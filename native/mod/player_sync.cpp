#include "player_sync.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "audio_probe.hpp"
#include "body_choice.hpp"
#include "coop_session.hpp"
#include "self_voice.hpp"
#include "partner_sound.hpp"
#include "partner_voice.hpp"

#include "debug_actions.hpp"
#include "face_sync.hpp"
#include "game_api.hpp"
#include "link.hpp"
#include "log.hpp"
#include "nametags.hpp"
#include "overlay/overlay_input.hpp"
#include "partner_collision.hpp"
#include "partner_gear.hpp"
#include "partner_holo.hpp"
#include "partner_source.hpp"
#include "pose_wire.hpp"
#include "real_body.hpp"
#include "status.hpp"
#include "steam_owner.hpp"
#include "partner_tackle.hpp"
#include "enemy_sync.hpp"
#include "shot_sync.hpp"
#include "ride_together.hpp"
#include "save_guard.hpp"
#include "vehicle_drive.hpp"
#include "vehicle_share.hpp"
#include "world_share.hpp"
#include "vehicle_sync.hpp"
#include "suit_colour.hpp"
#include "fragile_textures.hpp"
#include "texture_share.hpp"
#include "automation.hpp"

namespace st::sync {
namespace {

using game::Entity;
using game::WorldTransform;

constexpr std::uintptr_t kEntityParent = 0x70;  // Entity: parent entity (vehicle, mount), as game_actions reads it

// ---------------------------------------------------------------- snapshot ring
// Remote poses keyed by the sender's millisecond clock. Playback runs at
// (now - offset - delay) on the sender's clock, where offset is a smoothed
// sliding-window minimum of (receive_ms - sender_ms): the minimum is the
// fastest-delivered packet, so jitter only ever adds delay, never time travel.
struct Snap {
    std::int64_t sender_ms;  // unwrapped sender clock
    double recv_ms;          // our clock
    double x, y, z;
    wire::Quat q;
};

class Ring {
public:
    void Reset() {
        count_ = head_ = 0;
        have_offset_ = false;
        last_sender_ = wraps_ = 0;
    }
    // False when the sample is older than the newest one (reordered / old session).
    bool Push(std::uint32_t sender32, double recv_ms, double x, double y, double z, const wire::Quat& q) {
        std::uint32_t wraps = wraps_;
        if (count_ && sender32 < last_sender_ && last_sender_ - sender32 > 0x80000000u) ++wraps;
        const std::int64_t s = (static_cast<std::int64_t>(wraps) << 32) | sender32;
        if (count_ && s <= At(count_ - 1).sender_ms) return false;
        wraps_ = wraps;
        last_sender_ = sender32;
        Snap& slot = data_[(head_ + count_) % kCap];
        slot = Snap{s, recv_ms, x, y, z, q};
        if (count_ < kCap) ++count_;
        else head_ = (head_ + 1) % kCap;
        // Target offset: minimum over the last 3 s of samples.
        double target = recv_ms - static_cast<double>(s);
        for (unsigned i = count_; i-- > 0;) {
            const Snap& o = At(i);
            if (recv_ms - o.recv_ms > 3000.0) break;
            const double off = o.recv_ms - static_cast<double>(o.sender_ms);
            if (off < target) target = off;
        }
        target_offset_ = target;
        if (!have_offset_) {
            offset_ = target;
            have_offset_ = true;
        }
        return true;
    }
    // Called once per frame: glide the playback offset toward the target
    // (snap if it is far off, e.g. after a route change).
    void Settle() {
        if (!have_offset_) return;
        const double d = target_offset_ - offset_;
        offset_ = std::fabs(d) > 250.0 ? target_offset_ : offset_ + d * 0.05;
    }
    bool Empty() const { return count_ == 0; }
    // The sender-clock time Sample() plays at (joint frames use the same timeline).
    double PlayTime(double now_ms, double delay_ms) const { return now_ms - offset_ - delay_ms; }
    // A sender timestamp on the ring's unwrapped axis (for frames that ride with the pose).
    double Unwrapped(std::uint32_t sender32) const {
        std::uint32_t w = wraps_;
        if (count_ && sender32 < last_sender_ && last_sender_ - sender32 > 0x80000000u) ++w;
        else if (count_ && sender32 > last_sender_ && sender32 - last_sender_ > 0x80000000u && w) --w;
        return static_cast<double>((static_cast<std::int64_t>(w) << 32) | sender32);
    }
    bool Sample(double now_ms, double delay_ms, double (&pos)[3], wire::Quat& q) const {
        if (!count_) return false;
        const double t = now_ms - offset_ - delay_ms;
        const Snap& newest = At(count_ - 1);
        if (t >= static_cast<double>(newest.sender_ms) || count_ == 1) {
            pos[0] = newest.x, pos[1] = newest.y, pos[2] = newest.z;
            q = newest.q;
            return true;
        }
        const Snap& oldest = At(0);
        if (t <= static_cast<double>(oldest.sender_ms)) {
            pos[0] = oldest.x, pos[1] = oldest.y, pos[2] = oldest.z;
            q = oldest.q;
            return true;
        }
        for (unsigned i = count_ - 1; i > 0; --i) {
            const Snap& b = At(i);
            const Snap& a = At(i - 1);
            if (static_cast<double>(a.sender_ms) <= t && t <= static_cast<double>(b.sender_ms)) {
                const double span = static_cast<double>(b.sender_ms - a.sender_ms);
                const double f = span > 0 ? (t - static_cast<double>(a.sender_ms)) / span : 1.0;
                const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                if (dx * dx + dy * dy + dz * dz > 25.0 * 25.0) {  // teleport: no sliding across the map
                    pos[0] = b.x, pos[1] = b.y, pos[2] = b.z;
                    q = b.q;
                    return true;
                }
                pos[0] = a.x + dx * f, pos[1] = a.y + dy * f, pos[2] = a.z + dz * f;
                q = wire::Slerp(a.q, b.q, static_cast<float>(f));
                return true;
            }
        }
        return false;
    }

private:
    static constexpr unsigned kCap = 512;  // > 15 s at 30 Hz (echo needs a few seconds)
    const Snap& At(unsigned i) const { return data_[(head_ + i) % kCap]; }
    Snap data_[kCap];
    unsigned head_ = 0, count_ = 0;
    double offset_ = 0, target_offset_ = 0;
    bool have_offset_ = false;
    std::uint32_t last_sender_ = 0, wraps_ = 0;
};

// ---------------------------------------------------------------- state
Config g_cfg;
bool g_net = false;  // host/guest (vs echo)
Ring g_ring;
std::uint32_t g_generation = 0;

// Real character bodies (docs/real-partner-body.md "Character choice"): what we send as
// STJ1 body_id, and the peer's body_id after the stable-for-30-frames debounce.
const bodies::Body* g_own_body = nullptr;
bodies::StableId g_peer_body;  // Known() also means: the peer sent playable STJ1 in this link session

// Joint-frame starvation (docs/real-partner-body.md "Fallback when the peer sends no STJ1").
// A peer behind a pause or a notice sends positions but no joints (its animate never runs), so
// starvation is not proof of a hologram peer: after a starvation fallback the real body comes
// back once joint frames flow again, and a body that has had joints holds its last pose while
// the peer stands still.
bool g_starved_fallback = false;     // the chain left 'real' because no joint frames came
double g_last_joints_ms = 0;         // our clock at the newest received (or echoed) playable STJ1 frame
unsigned g_joints_since_fallback = 0;
double g_rx_pos[3] = {};             // the newest received transform's position
bool g_rx_in_vehicle = false;        // the newest received pose has kPoseInVehicle (player collision)
std::uint32_t g_rx_flags = 0;        // the newest received pose's STP1 flags (riding together: kPosePassenger)
double g_joints_pos[3] = {};         // ... when the newest playable STJ1 frame arrived
// When the real body began waiting for the session's first joint frame (TrySpawn). Its own timer,
// cleared whenever that wait is interrupted (link down, partner silent, Sam asleep, new link
// session): a stale start would otherwise fall back to the hologram at once when positions resume.
double g_first_joints_wait_ms = 0;

Entity* g_local = nullptr;
game::Guid g_local_guid;
WorldTransform g_local_wt{};
bool g_local_valid = false;
bool g_local_active = false;
double g_prev_pos[3] = {};
double g_prev_pos_ms = 0;
float g_speed = 0;
double g_move_dir[2] = {0, 0};  // the local player's newest horizontal walking direction (unit; collision tests)

// action collision 3: the partner pinned in place (collision tests).
bool g_pin = false;
WorldTransform g_pin_wt{};

double g_local_since_ms = 0;        // when the current local player entity appeared
double g_local_asleep_since_ms = 0;  // 0 while Sam is awake

// ---- the partner's body (docs/partner-hologram.md)
// holo:   the welcome/collectors hologram (partner_holo.cpp), spawned asynchronously;
// marker: a trace-point prop, created synchronously into g_slot;
// echo:   the echo resource (invisible, voice-capable), created into g_slot.
// The body in use walks down g_chain when one cannot be found or spawned.
PartnerBody g_chain[4] = {PartnerBody::holo, PartnerBody::marker, PartnerBody::echo, PartnerBody::echo};
unsigned g_chain_len = 3;
unsigned g_chain_pos = 0;

// A synchronously created, non-player entity (marker or echo body, or the voice echo).
struct Slot {
    const char* role = "partner";
    partner::Source src;
    Entity* e = nullptr;
    std::uintptr_t vt = 0;
    game::Guid guid;
};
Slot g_slot;   // marker / echo body
Slot g_voice;  // partner_voice=1: the echo next to a holo/marker body
double g_next_voice_ms = 0;

const char* g_remote_state = "none";
unsigned g_spawn_failures = 0;
unsigned g_holo_failures = 0;
bool g_spawn_disabled = false;
bool g_chain_exhausted = false;  // every body failed; F7 / respawn starts over
double g_next_spawn_ms = 0;
double g_body_wait_since_ms = 0;  // when the current body started waiting for its data
double g_next_wait_log_ms = 0;
double g_proxy_check_ms = 0;  // one-shot diagnostics 1 s after spawn
double g_last_remote_ms = 0;  // our clock when the newest accepted remote pose arrived
double g_link_down_since_ms = 0;
double g_next_link_check_ms = 0;

double g_next_send_ms = 0;
std::uint32_t g_seq = 0;
double g_next_status_ms = 0;
double g_next_trace_ms = 0;
double g_next_traffic_ms = 0;
std::uint64_t g_frames = 0;
double g_first_tick_ms = 0;
bool g_logged_rate = false;
bool g_prev_f7 = false, g_prev_f6 = false;

// Debug menu (debug_actions.cpp): spawning held by "Remove partner", the newest
// interpolated partner target, and a 1 s tick-rate window.
bool g_spawn_held = false;
bool g_last_target_valid = false;
double g_last_target[3] = {};
double g_rate_window_ms = 0;
std::uint64_t g_rate_window_frames = 0;
float g_tick_hz = 0;

PartnerBody CurrentBody() { return g_chain[g_chain_pos]; }

std::uint64_t Flags(Entity* e) {
    std::uint64_t f = 0;
    return e && partner::ReadFlags(e, &f) ? f : 0;
}

// The partner must never change the real Sam's sleep state (flag 0x100).
bool SamPutToSleep(std::uint64_t before, std::uint64_t after) {
    return !(before & game::kEntityFlagSleeping) && (after & game::kEntityFlagSleeping);
}

// ---------------------------------------------------------------- slots
// Same object we created: vtable AND the per-creation GUID still match.
bool SlotAlive(const Slot& s) {
    if (!s.e) return false;
    std::uintptr_t vt = 0;
    game::Guid g;
    return partner::ReadVtable(s.e, &vt) && vt == s.vt && partner::ReadGuid(s.e, &g) && g == s.guid;
}

void SlotForget(Slot& s, const char* why) {
    if (s.e) Log("[sync] forgetting %s entity %p (%s)", s.role, s.e, why);
    s.e = nullptr;
    s.vt = 0;
    s.guid = {};
}

void SlotRemove(Slot& s, const char* why) {
    if (!s.e) return;
    if (!SlotAlive(s)) {
        SlotForget(s, "already gone");
        return;
    }
    const std::uint64_t sam_before = Flags(g_local);
    const bool ok = partner::Remove(s.e);
    const std::uint64_t sam_after = Flags(g_local);
    Log("[sync] removed %s entity %p (%s)%s; Sam flags 0x%llX -> 0x%llX%s", s.role, s.e, why, ok ? "" : " - FAULTED",
        static_cast<unsigned long long>(sam_before), static_cast<unsigned long long>(sam_after),
        SamPutToSleep(sam_before, sam_after) ? "  <-- Sam was put to sleep!" : "");
    if (!ok) g_spawn_disabled = true;  // half-torn-down state: never try again this run
    SlotForget(s, why);
}

enum class SlotSpawnResult { spawned, wait, retry_later, fatal };

SlotSpawnResult SlotSpawn(Slot& s, partner::Kind kind, const WorldTransform& at, const char* name) {
    if (s.src.res && s.src.kind != kind) partner::Release(s.src);
    if (!s.src.res) {
        const bool ok = kind == partner::Kind::marker ? partner::AcquireMarker(&s.src, g_cfg.partner_marker)
                                                      : partner::Acquire(&s.src);
        if (!ok) return SlotSpawnResult::wait;
    }
    const std::uint64_t sam_before = Flags(g_local);
    Entity* e = nullptr;
    switch (partner::Create(s.src, at, name, &e)) {
        case partner::CreateResult::ok:
            break;
        case partner::CreateResult::busy:
            return SlotSpawnResult::wait;  // the game is using the echo manager this instant: next frame
        case partner::CreateResult::unloaded:
            partner::Release(s.src);
            return SlotSpawnResult::wait;
        case partner::CreateResult::faulted:
            Log("[sync] %s spawn FAULTED inside CreateEntity (%s resource=%p)", s.role, s.src.label, s.src.res);
            return SlotSpawnResult::fatal;
        case partner::CreateResult::null_entity:
            Log("[sync] %s spawn from the %s resource returned no entity", s.role, s.src.label);
            return SlotSpawnResult::retry_later;
    }
    s.e = e;
    partner::ReadVtable(e, &s.vt);
    partner::ReadGuid(e, &s.guid);
    partner::DescribeEntity(e, s.role);
    std::uint64_t f = Flags(e);
    bool shown = true, awake = true;
    if (!(f & 0x2)) shown = partner::SetVisible(e, true);
    if (g_cfg.partner_activate) awake = partner::WakeIfAsleep(e);
    const std::uint64_t sam_after = Flags(g_local);
    f = Flags(e);
    Log("[sync] %s spawned from the %s resource: entity=%p at (%.2f, %.2f, %.2f) flags now 0x%llX "
        "(set visible %s, wake %s)",
        s.role, s.src.label, e, at.pos.x, at.pos.y, at.pos.z, static_cast<unsigned long long>(f),
        shown ? "ok" : "unavailable", g_cfg.partner_activate ? (awake ? "ok" : "not possible") : "skipped by config");
    Log("[sync]   Sam flags 0x%llX -> 0x%llX%s", static_cast<unsigned long long>(sam_before),
        static_cast<unsigned long long>(sam_after), SamPutToSleep(sam_before, sam_after) ? "  <-- Sam was put to sleep!" : "");
    return SlotSpawnResult::spawned;
}

// ---------------------------------------------------------------- body
bool RealOurs() { return realbody::GetOwner() == realbody::Owner::partner && realbody::GetPhase() != realbody::Phase::idle; }
bool BodyPresent() { return g_slot.e != nullptr || holo::GetPhase() != holo::Phase::idle || RealOurs(); }

// The entity that is the partner's body right now (validated alive), or nullptr.
Entity* BodyEntity() {
    if (Entity* h = holo::LiveEntity()) return h;
    if (realbody::GetOwner() == realbody::Owner::partner)
        if (Entity* r = realbody::LiveEntity()) return r;
    return SlotAlive(g_slot) ? g_slot.e : nullptr;
}

bool OurEntity(Entity* e) {
    return e && (e == g_slot.e || e == g_voice.e || e == holo::LiveEntity() || e == realbody::LiveEntity() ||
                 e == realbody::SelfEntity());
}

void RemoveBody(const char* why) {
    const bool had = BodyPresent() || g_voice.e;
    holo::Remove(why);
    if (RealOurs()) realbody::Remove(why);
    SlotRemove(g_slot, why);
    SlotRemove(g_voice, why);
    if (had) g_remote_state = "none";
    g_proxy_check_ms = 0;
}

void ReleaseSources() {
    partner::Release(g_slot.src);
    partner::Release(g_voice.src);
}

void BuildChain() {
    g_chain_len = 0;
    switch (g_cfg.partner_body) {
        case PartnerBody::real:
            g_chain[g_chain_len++] = PartnerBody::real;
            [[fallthrough]];
        case PartnerBody::holo:
            g_chain[g_chain_len++] = PartnerBody::holo;
            [[fallthrough]];
        case PartnerBody::marker:
            g_chain[g_chain_len++] = PartnerBody::marker;
            [[fallthrough]];
        case PartnerBody::echo:
            g_chain[g_chain_len++] = PartnerBody::echo;
            break;
    }
    g_chain_pos = 0;
}

void ResetBodyChain() {
    g_chain_pos = 0;
    g_starved_fallback = false;
    g_holo_failures = 0;
    g_spawn_failures = 0;
    g_body_wait_since_ms = 0;
    g_first_joints_wait_ms = 0;
    holo::ResetRejected();
    realbody::ResetRefused();
}

void FallBack(const char* why, double now) {
    const PartnerBody from = CurrentBody();
    RemoveBody("falling back to another body");
    partner::Release(g_slot.src);
    g_body_wait_since_ms = 0;
    g_spawn_failures = 0;
    g_holo_failures = 0;
    if (g_chain_pos + 1 < g_chain_len) {
        ++g_chain_pos;
        Log("[sync] partner body '%s' unavailable (%s): falling back to '%s'", PartnerBodyName(from), why,
            PartnerBodyName(CurrentBody()));
        g_next_spawn_ms = now;
    } else {
        Log("[sync] partner body '%s' unavailable (%s) and no other body is left; spawning stopped (F7 retries)",
            PartnerBodyName(from), why);
        g_remote_state = "failed";
        g_chain_exhausted = true;
    }
}

// Waiting for a body's game data (collectors DB, trace-point config): log
// now and then, fall back after `limit_ms`.
void WaitOrFallBack(const char* why, double now, double limit_ms) {
    if (g_body_wait_since_ms == 0) g_body_wait_since_ms = now;
    if (now - g_body_wait_since_ms > limit_ms) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s for %.0f s", why, limit_ms / 1000.0);
        FallBack(buf, now);
        return;
    }
    if (now >= g_next_wait_log_ms) {
        g_next_wait_log_ms = now + 5000;
        Log("[sync] partner body '%s': %s; waiting", PartnerBodyName(CurrentBody()), why);
    }
    g_next_spawn_ms = now + 1000;
}

void BodySpawned(double now) {
    g_spawn_failures = 0;
    g_body_wait_since_ms = 0;
    g_proxy_check_ms = now + 1000;
    g_next_voice_ms = now + 1000;
}

void ComposeTransform(const double (&pos)[3], const wire::Quat& q, WorldTransform& out) {
    out.pos.x = pos[0];
    out.pos.y = pos[1];
    out.pos.z = pos[2];
    wire::MatrixFromQuat(q, out.rot.m);
}

// The real body the partner should have now: partner_character (debug override), else the
// peer's debounced body_id when it is offered, else Sam; a refused body falls back to Sam.
// nullptr when every candidate was refused (the chain then falls back to the hologram).
const bodies::Body* RealBodyToShow() {
    const bodies::PartnerChoice c = bodies::ChoosePartner(g_cfg.partner_character.c_str(), g_peer_body.Known(), g_peer_body.Shown());
    if (!realbody::Refused(c.body->id)) return c.body;
    return !realbody::Refused(bodies::Default().id) ? &bodies::Default() : nullptr;
}

// Every received (or echoed) STJ1 frame the body can play (realbody::PushFrame accepted it):
// the body_id debounce, the joint-starvation state, and a log line when the raw id changes (the
// first few times only, so a flapping peer cannot flood the log) and when the shown character
// changes. A frame the ring dropped (another skeleton layout or joint count, or reordered) must
// not count: a real body spawned or brought back on such frames would have nothing to play.
void PeerBodyId(std::uint8_t id) {
    static std::uint8_t logged_raw = 0;
    static unsigned raw_logs = 0;
    if ((!g_peer_body.Known() || id != logged_raw) && raw_logs < 20) {
        ++raw_logs;
        logged_raw = id;
        const bodies::Body* b = bodies::FindById(id);
        Log("[sync] the partner sends joint frames (STJ1), body_id %u (%s)", id, b ? b->key : "unknown");
    }
    g_last_joints_ms = link::NowMsPrecise();
    ++g_joints_since_fallback;
    std::memcpy(g_joints_pos, g_rx_pos, sizeof(g_joints_pos));
    const bool first = !g_peer_body.Known();
    if (!g_peer_body.Push(id)) return;
    const bodies::PartnerChoice c = bodies::ChoosePartner(g_cfg.partner_character.c_str(), true, id);
    char when[48];
    if (first) std::snprintf(when, sizeof(when), "first frame");
    else std::snprintf(when, sizeof(when), "stable for %u frames", bodies::kStableFrames);
    Log("[sync] partner character: '%s' (peer body_id %u, %s%s)", c.body->key, id, when,
        c.from_override      ? "; the partner_character override wins"
        : c.peer_not_offered ? "; not an offered body, shown as sam"
                             : "");
}

// A received or echoed STJ1 frame: into the body's ring and, only if it was kept, into the
// "peer sends joints" state. The first few drops are logged (another layout, or reordered).
void TakeJointFrame(const wire::JointFrame& f, double key_ms) {
    if (realbody::PushFrame(f, key_ms)) {
        PeerBodyId(f.body_id);
        if (f.flags & wire::kJointsOutfit) realbody::SetPartnerOutfit(&f.outfit);
        if (f.flags & wire::kJointsFace) facesync::SetPartnerFace(&f.face);
        return;
    }
    static unsigned dropped_logs = 0;
    if (dropped_logs < 5) {
        ++dropped_logs;
        Log("[sync] joint frame seq %u dropped: %u joints, layout %08X (playable: 77 joints, layout %08X, newer than the last)",
            f.seq, static_cast<unsigned>(f.joint_count), f.layout_hash, wire::kSamLayoutHash);
    }
}

// Host/guest: every 10 s while the link carries poses, what went each way and the payload rate
// (docs/real-partner-body.md: about 560 B per datagram at 30 Hz with joint frames).
void LogTraffic(double now) {
    if (!g_net || now < g_next_traffic_ms) return;
    static link::Info prev{};
    static double prev_ms = 0;
    const link::Info li = link::GetInfo();
    g_next_traffic_ms = now + 10000;
    const double s = prev_ms > 0 ? (now - prev_ms) / 1000.0 : 0;
    const std::uint64_t tx = li.tx_frames - prev.tx_frames, txj = li.tx_joint_frames - prev.tx_joint_frames,
                        rx = li.rx_frames - prev.rx_frames, rxj = li.rx_joint_frames - prev.rx_joint_frames,
                        bytes = li.tx_payload_bytes - prev.tx_payload_bytes, txg = li.tx_gear_frames - prev.tx_gear_frames,
                        rxg = li.rx_gear_frames - prev.rx_gear_frames;
    prev = li;
    prev_ms = now;
    if (s <= 0 || (!tx && !rx)) return;
    Log("[sync] link traffic over %.0f s: sent %llu poses (%llu with joint frames), %.0f B/s payload (%.0f B each); "
        "received %llu poses, %llu joint frames; gear frames (STC1) sent %llu, received %llu",
        s, static_cast<unsigned long long>(tx), static_cast<unsigned long long>(txj), static_cast<double>(bytes) / s,
        tx ? static_cast<double>(bytes) / static_cast<double>(tx) : 0.0, static_cast<unsigned long long>(rx),
        static_cast<unsigned long long>(rxj), static_cast<unsigned long long>(txg), static_cast<unsigned long long>(rxg));
}

// The real body has no joint frames to play: fall back to the next body (the hologram), and
// remember why, so ReturnToReal can bring the real body back when joint frames arrive.
void StarvedFallBack(const char* why, double now) {
    FallBack(why, now);
    g_first_joints_wait_ms = 0;
    if (CurrentBody() == PartnerBody::real) return;  // nothing to fall back to
    g_starved_fallback = true;
    g_joints_since_fallback = 0;
}

// After a starvation fallback: the peer sends joint frames again (it left a pause or a notice,
// or its capture started late). 30 fresh frames (about 1 s) before switching, so one stray
// frame does not swap the body.
void ReturnToReal(double now) {
    if (!g_starved_fallback || CurrentBody() == PartnerBody::real) return;
    if (g_joints_since_fallback < bodies::kStableFrames || now - g_last_joints_ms > 500) return;
    if (!realbody::Available() || !RealBodyToShow()) {
        g_starved_fallback = false;  // the real body cannot come back this run
        return;
    }
    Log("[sync] the partner sends joint frames (STJ1) again: back to the real body (from '%s')",
        PartnerBodyName(CurrentBody()));
    RemoveBody("the partner sends joint frames again");
    partner::Release(g_slot.src);
    g_chain_pos = 0;
    g_chain_exhausted = false;
    g_starved_fallback = false;
    g_holo_failures = 0;
    g_spawn_failures = 0;
    g_body_wait_since_ms = 0;
    g_next_spawn_ms = now;
}

// A live real body whose joint frames stopped: while the peer stands still (paused, in a menu)
// it holds the last pose; once the peer moves without joints (> 1 m) it would glide in that
// pose, so it falls back. A peer that never sent joints this session falls back at once.
bool PeerMovedSinceJoints() {
    if (!g_peer_body.Known()) return true;
    const double dx = g_rx_pos[0] - g_joints_pos[0], dy = g_rx_pos[1] - g_joints_pos[1], dz = g_rx_pos[2] - g_joints_pos[2];
    return dx * dx + dy * dy + dz * dz > 1.0;
}

// The peer's body_id changed (after the debounce): respawn a real body that no longer matches.
// Only bodies we own; a debug body in the slot or a hologram after a fallback stay as they are.
void FollowPeerCharacter(double now) {
    if (!RealOurs() || CurrentBody() != PartnerBody::real || !realbody::CurrentBody()) return;
    const bodies::Body* want = RealBodyToShow();
    if (!want || want == realbody::CurrentBody()) return;
    Log("[sync] partner character now '%s' (was '%s'): respawning the real body", want->key, realbody::CurrentBody()->key);
    RemoveBody("the partner's character changed");
    g_next_spawn_ms = now;
}

// Cloning Sam's own DSPlayerEntityResource crashes the game (it needs a
// DSPlayerController; first in-game run 2026-11-16), so the partner is always
// a non-player entity: see the body chain above.
void TrySpawn(const WorldTransform& at, double now) {
    if (g_spawn_disabled || g_spawn_held || g_chain_exhausted || now < g_next_spawn_ms || !g_local) return;
    if (!g_local_active) return;                // never spawn while Sam is asleep (loading, cutscene, knot space)
    if (partner::GameRemovingEchoes()) {
        static double logged_ms = -1e9;
        if (now - logged_ms > 30000) {
            logged_ms = now;
            Log("[sync] not spawning: the game's echo teardown condition is set");
        }
        return;
    }
    if (now - g_local_since_ms < 2000) return;  // the player entity is swapped a few times while a save loads
    if (g_net && g_link_down_since_ms > 0) return;  // session ended / reconnecting: no body until the link is back

    char why[200] = {};
    switch (CurrentBody()) {
        case PartnerBody::real: {
            if (realbody::GetPhase() != realbody::Phase::idle) return;  // DriveReal polls it (or a debug body holds the slot)
            // No joint frame yet in this session: a body now would stand in its bind pose, and
            // it might be the wrong character (the body_id comes with STJ1). Wait 3 s, then the
            // hologram stands in (ReturnToReal brings the real body when joints arrive).
            if (!g_peer_body.Known()) {
                if (g_first_joints_wait_ms == 0) {
                    g_first_joints_wait_ms = now;
                    Log("[sync] partner body 'real': waiting for the partner's first joint frame (STJ1)");
                }
                if (now - g_first_joints_wait_ms > 3000)
                    StarvedFallBack("the partner sends positions but no joint frames (STJ1) for 3 s", now);
                else
                    g_next_spawn_ms = now + 250;
                return;
            }
            const bodies::Body* body = RealBodyToShow();
            if (!body) {
                FallBack("every real body was refused", now);
                return;
            }
            switch (realbody::Begin(*body, at, realbody::Owner::partner, now, why, sizeof(why))) {
                case realbody::BeginResult::started:
                    g_remote_state = "loading";
                    return;
                case realbody::BeginResult::busy:
                    g_next_spawn_ms = now + 1000;
                    return;
                case realbody::BeginResult::unavailable:
                    FallBack(why, now);
                    return;
                case realbody::BeginResult::failed:
                    Log("[sync] real body spawn failed: %s", why);
                    if (++g_holo_failures >= 3) FallBack(why, now);
                    else g_next_spawn_ms = now + 2000;
                    return;
            }
            return;
        }
        case PartnerBody::holo: {
            if (holo::GetPhase() != holo::Phase::idle) return;  // Poll drives it
            switch (holo::Begin(at, g_cfg.partner_holo, now, why, sizeof(why))) {
                case holo::BeginResult::started:
                    g_remote_state = "loading";
                    return;
                case holo::BeginResult::waiting_db:
                    WaitOrFallBack(why, now, 30000);
                    return;
                case holo::BeginResult::no_item:
                case holo::BeginResult::unavailable:
                    FallBack(why, now);
                    return;
                case holo::BeginResult::failed:
                    Log("[sync] hologram spawn failed: %s", why);
                    if (++g_holo_failures >= 3) FallBack(why, now);
                    else g_next_spawn_ms = now + 2000;
                    return;
            }
            return;
        }
        case PartnerBody::marker:
        case PartnerBody::echo: {
            const bool marker = CurrentBody() == PartnerBody::marker;
            switch (SlotSpawn(g_slot, marker ? partner::Kind::marker : partner::Kind::echo, at, "StrandedTogetherPartner")) {
                case SlotSpawnResult::spawned:
                    g_remote_state = marker ? "visible" : "spawned";
                    BodySpawned(now);
                    return;
                case SlotSpawnResult::wait:
                    if (marker) WaitOrFallBack("trace-point props not available", now, 20000);
                    else g_next_spawn_ms = now + 2000;
                    return;
                case SlotSpawnResult::retry_later:
                    ++g_spawn_failures;
                    g_remote_state = "failed";
                    if (marker && g_spawn_failures >= 3) FallBack("the marker prop spawned no entity 3 times", now);
                    else g_next_spawn_ms = now + (g_spawn_failures < 3 ? 5000 : 60000);
                    return;
                case SlotSpawnResult::fatal:
                    if (marker) {
                        FallBack("CreateEntity faulted on the marker prop", now);
                    } else {
                        g_spawn_disabled = true;
                        g_remote_state = "failed";
                        Log("[sync] partner spawning disabled for this run");
                    }
                    return;
            }
            return;
        }
    }
}

// Per frame: drive the asynchronous hologram (load -> realize -> visible).
void DriveHolo(double now) {
    if (holo::GetPhase() == holo::Phase::idle) return;
    char why[200] = {};
    switch (holo::Poll(now, why, sizeof(why))) {
        case holo::PollResult::idle:
            return;
        case holo::PollResult::loading:
            g_remote_state = "loading";
            return;
        case holo::PollResult::became_live:
            g_remote_state = "visible";
            g_holo_failures = 0;
            BodySpawned(now);
            Log("[sync] partner body: hologram %08X", holo::CurrentHash());
            return;
        case holo::PollResult::live:
            holo::Maintain(now);
            return;
        case holo::PollResult::failed:
            g_remote_state = "failed";
            if (!holo::Available()) FallBack(why, now);
            else if (++g_holo_failures >= 3) FallBack(why, now);
            else g_next_spawn_ms = now + 1000;  // Begin picks the next item (refused ones are skipped)
            return;
    }
}

// Per frame: drive the asynchronous real body (load -> realize -> validate -> live).
void DriveReal(double now) {
    if (!RealOurs()) return;
    char why[200] = {};
    switch (realbody::Poll(now, why, sizeof(why))) {
        case realbody::PollResult::idle:
        case realbody::PollResult::live:
            return;
        case realbody::PollResult::loading:
            g_remote_state = "loading";
            return;
        case realbody::PollResult::became_live:
            g_remote_state = "visible";
            g_holo_failures = 0;
            BodySpawned(now);
            Log("[sync] partner body: real '%s'", realbody::CurrentBody() ? realbody::CurrentBody()->key : "?");
            return;
        case realbody::PollResult::failed:
            g_remote_state = "failed";
            if (!realbody::Available()) FallBack(why, now);
            else g_next_spawn_ms = now + 1000;  // the next Begin skips refused bodies (then falls back)
            return;
    }
}

// partner_voice=1 or voice_sync=1: the invisible echo rides along with the partner body (voice_sync: at its head,
// pvoice::EmitterTransform), the partner voice's talking entity.
void DriveVoice(const WorldTransform& at, double now) {
    if (!(g_cfg.partner_voice || g_cfg.voice_sync) || CurrentBody() == PartnerBody::echo || g_voice.e || now < g_next_voice_ms) return;
    if (!BodyEntity() || !g_local_active || g_spawn_disabled) return;
    if (SlotSpawn(g_voice, partner::Kind::echo, at, "StrandedTogetherPartnerVoice") != SlotSpawnResult::spawned)
        g_next_voice_ms = now + 30000;
}

// The co-op features this game has off, with the reason ("" = all on, or not a host / guest): the overlay notice
// (coop::NoticeTick), link-status.json "features_off" and through it the launcher's Play / In game pages. Never silent
// (the v0.18.0 Local test ran without host_save and nothing on screen said the vehicles were the old copies).
void FeaturesOff(char* out, std::size_t cap) {
    out[0] = 0;
    if (g_cfg.role != Role::host && g_cfg.role != Role::guest) return;
    const auto real = [](const char* r) { return r && r[0] && std::strcmp(r, "not installed") != 0; };
    if (!g_cfg.host_save) {
        std::snprintf(out, cap, "host_save=0 (the launcher's Debug > Host holds the save is off): no waiting screen, no shared vehicles, "
                                "no riding together, no vehicle hits (the partner's vehicles are the old copies)");
        return;
    }
    if (!coop::Active()) {
        std::snprintf(out, cap, "the co-op save session is off (%s): no waiting screen, no shared vehicles, no riding together, no vehicle hits",
                      coop::OffReason());
        return;
    }
    if (real(vshare::OffReason())) {
        std::snprintf(out, cap, "shared vehicles (%s), so no riding together or vehicle hits", vshare::OffReason());
        return;
    }
    std::size_t n = 0;
    if (real(ride::OffReason())) n += std::snprintf(out + n, cap - n, "riding together (%s)", ride::OffReason());
    if (n < cap && real(vhit::OffReason())) n += std::snprintf(out + n, cap - n, "%svehicle hits (%s)", n ? "; " : "", vhit::OffReason());
    if (n < cap && real(shots::OffReason())) std::snprintf(out + n, cap - n, "%sshots (%s)", n ? "; " : "", shots::OffReason());
}

void PublishStatus(double now) {
    if (now < g_next_status_ms) return;
    g_next_status_ms = now + 1000;
    status::Snapshot s;
    s.role = RoleName(g_cfg.role);
    s.remote = g_remote_state;
    s.local_valid = g_local_valid;
    if (g_local_valid) {
        s.local_pos[0] = g_local_wt.pos.x, s.local_pos[1] = g_local_wt.pos.y, s.local_pos[2] = g_local_wt.pos.z;
    }
    if (g_net) {
        const link::Info li = link::GetInfo();
        s.state = li.state;
        s.transport = li.transport;
        s.route = li.route;
        std::strncpy(s.peer, li.peer, sizeof(s.peer) - 1);
        s.rtt_ms = li.rtt_ms;
        std::strncpy(s.reason, li.reason, sizeof(s.reason) - 1);
        if (!li.reason[0] && g_cfg.transport == Transport::steam && steam::GetPhase() == steam::Phase::failed)
            std::strncpy(s.reason, steam::FailReason(), sizeof(s.reason) - 1);
        if (g_cfg.role == Role::host) link::BuildJoinCode(s.code, sizeof(s.code));
        s.rx = li.rx_frames;
        s.tx = li.tx_frames;
    } else {
        s.state = "echo";
        s.transport = "";
    }
    FeaturesOff(s.features_off, sizeof(s.features_off));
    status::Publish(s);
}

void RespawnPartnerNow(double now, const char* why) {
    Log("[sync] %s: respawning the partner (body chain restarts at '%s')", why, PartnerBodyName(g_chain[0]));
    RemoveBody(why);
    ReleaseSources();
    ResetBodyChain();
    g_chain_exhausted = false;
    g_spawn_held = false;
    g_next_spawn_ms = now;
}

void LogPositionsNow() {
    const link::Info li = link::GetInfo();
    Entity* body = BodyEntity();
    Log("[sync] local=%p (%.2f, %.2f, %.2f) flags=0x%llX partner=%p (%s, %s) flags=0x%llX state=%s link=%s rtt=%u rx=%llu "
        "tx=%llu",
        g_local, g_local_wt.pos.x, g_local_wt.pos.y, g_local_wt.pos.z, static_cast<unsigned long long>(Flags(g_local)), body,
        PartnerBodyName(CurrentBody()), holo::PhaseName(), static_cast<unsigned long long>(body ? Flags(body) : 0),
        g_remote_state, g_net ? li.state : "echo", li.rtt_ms, static_cast<unsigned long long>(li.rx_frames),
        static_cast<unsigned long long>(li.tx_frames));
    Log("[sync]     rot rows: [%.3f %.3f %.3f] [%.3f %.3f %.3f] [%.3f %.3f %.3f]", g_local_wt.rot.m[0][0],
        g_local_wt.rot.m[0][1], g_local_wt.rot.m[0][2], g_local_wt.rot.m[1][0], g_local_wt.rot.m[1][1],
        g_local_wt.rot.m[1][2], g_local_wt.rot.m[2][0], g_local_wt.rot.m[2][1], g_local_wt.rot.m[2][2]);
    if (body) {
        WorldTransform pw{};
        if (game::TryGetTransform(body, &pw)) Log("[sync]     partner at (%.2f, %.2f, %.2f)", pw.pos.x, pw.pos.y, pw.pos.z);
    }
}

void Hotkeys(double now) {
    // Only react while the game window is focused, and not while the debug
    // menu has the keyboard (typing there must not trigger F6/F7). The REAL foreground: the Local test guest's
    // GetForegroundWindow is hooked to answer its own window (F6/F7 pressed in the host window fired in the guest too),
    // and a hidden run's keyboard is the user's (never).
    if (!overlay::input::RealForegroundIsOurs() || debug::MenuOpen()) {
        g_prev_f6 = g_prev_f7 = false;
        return;
    }
    const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f7 && !g_prev_f7) RespawnPartnerNow(now, "F7");
    if (f6 && !g_prev_f6) {
        Log("[sync] F6: positions");
        LogPositionsNow();
    }
    g_prev_f7 = f7;
    g_prev_f6 = f6;
}

// Tear the body down whenever the world it lives in may be going away.
void TeardownTriggers(double now) {
    // Loads, cutscenes and knot space put Sam to sleep first: take the partner
    // down then, so nothing of ours is outstanding when the game changes worlds.
    if (!g_local_active) g_body_wait_since_ms = g_first_joints_wait_ms = 0;  // waiting only counts while Sam is up
    if (BodyPresent() && g_local_asleep_since_ms > 0 && now - g_local_asleep_since_ms >= 1000) {
        RemoveBody("Sam asleep (load, cutscene or knot space)");
        g_next_spawn_ms = now + 2000;
    }
    if ((BodyPresent() || g_voice.e) && partner::GameRemovingEchoes()) {
        RemoveBody("the game is removing its echoes");
        g_next_spawn_ms = now + 2000;
    }
    if (holo::DbChanged()) {
        RemoveBody("collectors DB freed or replaced");
        g_next_spawn_ms = now + 2000;
    }
    for (Slot* s : {&g_slot, &g_voice}) {
        if (s->src.res && !partner::StillLoaded(s->src)) {
            char why[64];
            std::snprintf(why, sizeof(why), "%s config unloaded", s->src.label);
            if (s == &g_slot) RemoveBody(why);
            else SlotRemove(*s, why);
            partner::Release(s->src);
            g_next_spawn_ms = now + 2000;
        }
        if (s->e && !SlotAlive(*s)) {
            SlotForget(*s, "entity no longer valid (destroyed by the game?)");
            if (s == &g_slot) g_remote_state = "none";
            g_next_spawn_ms = now + 2000;
        }
    }
    // Session end (host/guest): the link dropped for 2 s.
    if (g_net && now >= g_next_link_check_ms) {
        g_next_link_check_ms = now + 250;
        const link::Info li = link::GetInfo();
        if (li.connected) {
            g_link_down_since_ms = 0;
        } else {
            if (g_link_down_since_ms == 0) g_link_down_since_ms = now;
            if (BodyPresent() && now - g_link_down_since_ms >= 2000) RemoveBody("link session ended");
        }
    }
}

}  // namespace

// The textures folder from session.ini (UTF-8). Hidden runs: only a folder inside the run's own output folder.
void ConfigureTextures(const Config& config, const std::wstring& mod_dir) noexcept {
    auto wide = [](const std::string& s) {
        std::wstring w;
        const int n = s.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        if (n > 0) {
            w.assign(static_cast<std::size_t>(n), L' ');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        }
        while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
        return w;
    };
    std::wstring dir = wide(config.textures_dir);
    if (automation::Enabled() && !dir.empty()) {
        // a hidden run never reads the player's own folder: only one inside the run's folder the supervisor names
        const std::wstring run = wide(config.automation_run_dir);
        const bool inside = !run.empty() && dir.size() > run.size() && dir[run.size()] == L'\\' &&
                            CompareStringOrdinal(dir.c_str(), static_cast<int>(run.size()), run.c_str(), static_cast<int>(run.size()), TRUE) == CSTR_EQUAL &&
                            dir.find(L"..") == std::wstring::npos;
        if (!inside) {
            Log("[tex] hidden run: textures_dir is not inside the run's folder (%s): ignored", run.empty() ? "no automation_run_dir" : "outside it");
            dir.clear();
        }
    }
    const bool on = config.fragile_textures && realbody::HookInstalled() && (config.role == Role::guest || config.role == Role::host || config.role == Role::solo);
    ftex::Settings s;
    s.on = on;
    s.hot_reload = config.textures_hot_reload;
    s.free_unused = config.textures_free;
    s.send = config.textures_send;
    s.receive = config.textures_receive;
    s.guest = config.role == Role::guest || config.role == Role::solo;
    s.dir = dir;
    s.defaults = config.textures_defaults;
    // the built textures' cache: never inside the textures folder (it may be synced to the cloud, and its watch would see
    // our own writes); a hidden run's next to the run's textures folder (the supervisor deletes both at the run's end)
    if (automation::Enabled()) s.cache = dir.empty() ? std::wstring() : dir + L"-cache";
    else s.cache = mod_dir + L"\\cache\\built-textures";
    ftex::Configure(s);
    texshare::Settings t;
    t.on = on && (config.role == Role::guest || config.role == Role::host);
    t.guest = config.role == Role::guest;
    t.send = config.textures_send;
    t.receive = config.textures_receive;
    t.tint = config.fragile_suit_colour;
    t.cache = (automation::Enabled() ? automation::OutDir() : mod_dir) + L"\\cache\\partner-textures";
    texshare::Configure(t);
}

void Init(const Config& config, const std::wstring& mod_dir) noexcept {
    g_cfg = config;
    g_net = config.role == Role::host || config.role == Role::guest;
    status::Init(mod_dir);
    g_ring.Reset();
    BuildChain();
    g_slot.role = "partner";
    g_voice.role = "partner voice";
    char chain[64] = {};
    for (unsigned i = 1; i < g_chain_len; ++i) {
        std::strncat(chain, i == 1 ? " (then " : ", ", sizeof(chain) - std::strlen(chain) - 1);
        std::strncat(chain, PartnerBodyName(g_chain[i]), sizeof(chain) - std::strlen(chain) - 1);
    }
    if (g_chain_len > 1) std::strncat(chain, ")", sizeof(chain) - std::strlen(chain) - 1);
    Log("[sync] init role=%s send_hz=%u interp=%ums activate=%d body=%s%s voice=%d%s", RoleName(config.role),
        config.send_hz, config.interp_delay_ms, config.partner_activate ? 1 : 0, PartnerBodyName(g_chain[0]), chain,
        config.partner_voice ? 1 : 0,
        config.role == Role::echo ? " (echo: your own movement is replayed by the partner body)" : "");

    // What the other player sees us as (STJ1 body_id). Sending needs the pose hook, which only a
    // partner_body=real (or pose_hook=1) game installs at start-up: a hologram game sends no
    // joint frames, so a real-body peer shows it as the hologram (player_sync's starvation fallback).
    g_peer_body.Reset();
    const bodies::OwnChoice own = bodies::ChooseOwn(config.role == Role::host, config.my_character.c_str());
    g_own_body = own.body;
    if (own.host_forced)
        Log("[sync] my_character=%s ignored: the host is always sam", config.my_character.c_str());
    else if (own.unknown_key)
        Log("[sync] my_character=%s is not a known character: sending sam", config.my_character.c_str());
    Log("[sync] my character: '%s' (body_id %u%s); joint frames (STJ1) %s", g_own_body->key, g_own_body->id,
        bodies::IsOffered(g_own_body->id) ? "" : ", not offered: the partner shows sam",
        config.role == Role::solo                                          ? "not sent (solo)"
        : realbody::HookInstalled()                                        ? "sent with every pose"
        : config.partner_body == PartnerBody::real || config.pose_hook    ? "not sent (the pose hook is unavailable)"
                                                                           : "not sent (no pose hook: partner_body is not real)");
    // Self view (docs/real-partner-body.md section 8): this player sees themself as their own
    // character too. Only for a character other than Sam (ChooseOwn already made the host Sam).
    realbody::ConfigureRoomOutfit(config.room_outfit_black);  // section 34: also with the self view off (STJ1 body_id)
    if (config.self_character) {
        const bool sam = !g_own_body || g_own_body->id == bodies::Default().id;
        if (sam)
            Log("[sync] self view: not needed (your character is sam%s)", own.host_forced || config.role == Role::host ? ", the host" : "");
        else if (!realbody::HookInstalled())
            Log("[sync] self view unavailable: the pose hook is not installed (partner_body=real or pose_hook=1 at start)");
        else {
            realbody::ConfigureSelfFit(config.self_fit);
            realbody::ConfigureSelfHeadgear(config.self_headgear);
            realbody::ConfigureSelfFeet(config.self_feet_hide);
            realbody::ConfigureSelf(g_own_body);
        }
    }
    if (realbody::HookInstalled()) suitcol::Configure(config.fragile_suit_colour);
    // round 5: the guest's own suit colour pick outlives the host's loads (her sandboxed copy of the host's save is never saved)
    suitcol::ConfigureGuestPick(realbody::HookInstalled() && (config.fragile_suit_colour || config.fragile_textures) && config.role == Role::guest && config.host_save, mod_dir);
    // round 6 T2: Fragile's edited textures (fragile_textures.hpp, texture_looks.hpp, texture_share.hpp; docs/textures.md)
    ConfigureTextures(config, mod_dir);
    if (realbody::HookInstalled())
        realbody::ConfigureSmooth(config.smooth_pose ? realbody::kSmoothAll : 0u, config.role == Role::echo,
                                  static_cast<double>(config.echo_delay_s) * 1000.0);
    nametags::Init(config);
    collision::Init(config);
    gear::Init(config);
    vsync::Init(config);
    if (!config.partner_character.empty()) {
        if (bodies::FindByKey(config.partner_character.c_str()))
            Log("[sync] partner_character=%s: debug override, the partner is shown as this body whatever it sends",
                config.partner_character.c_str());
        else
            Log("[sync] partner_character=%s is not a known character: ignored (the partner's own choice is shown)",
                config.partner_character.c_str());
    }
}

void Tick() noexcept {
    const double now = link::NowMsPrecise();
    ++g_frames;
    if (g_first_tick_ms == 0) {
        g_first_tick_ms = now;
        Log("[sync] first tick on thread %lu", GetCurrentThreadId());
    } else if (!g_logged_rate && now - g_first_tick_ms >= 10000) {
        g_logged_rate = true;
        Log("[sync] tick rate: %.1f Hz over 10 s (thread %lu)", g_frames * 1000.0 / (now - g_first_tick_ms),
            GetCurrentThreadId());
    }

    if (now - g_rate_window_ms >= 1000.0) {
        if (g_rate_window_ms > 0)
            g_tick_hz = static_cast<float>(static_cast<double>(g_frames - g_rate_window_frames) * 1000.0 / (now - g_rate_window_ms));
        g_rate_window_ms = now;
        g_rate_window_frames = g_frames;
    }

    if (g_net && g_cfg.transport == Transport::steam) steam::Pump();

    // ---- local player (identity = pointer + per-creation GUID)
    Entity* local = nullptr;
    game::TryGetLocalPlayer(&local);
    if (OurEntity(local)) local = g_local;  // never mistake our partner for the player
    game::Guid guid;
    if (local && !partner::ReadGuid(local, &guid)) local = nullptr;
    if (local != g_local || (local && guid != g_local_guid)) {
        Log("[sync] local player entity %p -> %p", g_local, local);
        // The World object lives for the whole process, so a partner we created
        // stays in it across reloads unless we remove it (the game removes its own
        // echoes explicitly too). Liveness (vtable + per-creation GUID) tells a
        // live partner from freed memory without risking a fault.
        RemoveBody("local player changed");
        g_local = local;
        g_local_guid = local ? guid : game::Guid{};
        g_local_since_ms = now;
        ReleaseSources();  // re-validate the resources for the new world
        g_local_valid = false;
        if (!g_net) {
            g_ring.Reset();
            realbody::ResetFrames();
        }
    }
    g_local_valid = g_local && game::TryGetTransform(g_local, &g_local_wt);
    g_local_active = g_local && (Flags(g_local) & game::kEntityFlagSleeping) == 0;
    realbody::Tick(g_local, g_local_active && g_local_valid, now);  // pose capture publication, debug bodies
    if (g_local_active || !g_local) g_local_asleep_since_ms = 0;
    else if (g_local_asleep_since_ms == 0) g_local_asleep_since_ms = now;

    if (g_local_valid && g_prev_pos_ms > 0 && now - g_prev_pos_ms > 1.0) {
        const double dx = g_local_wt.pos.x - g_prev_pos[0], dy = g_local_wt.pos.y - g_prev_pos[1],
                     dz = g_local_wt.pos.z - g_prev_pos[2];
        const float inst = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0 / (now - g_prev_pos_ms));
        g_speed = g_speed * 0.9f + inst * 0.1f;
    }
    if (g_local_valid && g_prev_pos_ms > 0 && now - g_prev_pos_ms > 1.0) {
        const double mx = g_local_wt.pos.x - g_prev_pos[0], my = g_local_wt.pos.y - g_prev_pos[1];
        const double m = std::sqrt(mx * mx + my * my);
        if (m * 1000.0 / (now - g_prev_pos_ms) > 1.0) g_move_dir[0] = mx / m, g_move_dir[1] = my / m;
    }
    if (g_local_valid) {
        g_prev_pos[0] = g_local_wt.pos.x, g_prev_pos[1] = g_local_wt.pos.y, g_prev_pos[2] = g_local_wt.pos.z;
        g_prev_pos_ms = now;
    }

    // ---- send / record (fixed cadence; skip ahead after a hitch instead of bursting)
    const double interval = 1000.0 / g_cfg.send_hz;
    if (now >= g_next_send_ms) {
        g_next_send_ms = (now - g_next_send_ms > interval) ? now + interval : g_next_send_ms + interval;
        // (vsync::SendMuted: hidden tests only, action vehicle_sync 10: the partner sees us silent while the link stays up)
        if (g_local_valid && g_local_active && g_cfg.role != Role::solo && !vsync::SendMuted(now)) {
            wire::Pose p;
            p.seq = ++g_seq;
            p.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(now));
            p.flags = wire::kPoseValid;
            // Riding something (Sam has a parent entity: vehicle, mount): the partner's player
            // collision takes our body's capsule out of its world (partner_collision.hpp).
            if (partner::mem::Q(reinterpret_cast<std::uintptr_t>(g_local) + kEntityParent) != 0) p.flags |= wire::kPoseInVehicle;
            // Carried as the partner's passenger (ride_together.hpp): kPosePassenger | kPoseInVehicle (its capsule out there too).
            p.flags |= ride::PoseFlags();
            p.x = g_local_wt.pos.x, p.y = g_local_wt.pos.y, p.z = g_local_wt.pos.z;
            p.rot = wire::QuatFromMatrix(g_local_wt.rot.m);
            p.speed = g_speed;
            // Joint pose (STJ1, docs/real-partner-body.md): Sam's captured skeleton pose rides with
            // the transform (same seq and timestamp). In echo it goes through the full encode ->
            // decode path, so the played-back animation is exactly what a peer would receive.
            static wire::JointFrame jf, back;
            double capture_ms = 0;
            const bool have_joints = realbody::HookInstalled() && realbody::CaptureFrame(&jf, &capture_ms);
            if (have_joints) {
                jf.seq = p.seq;
                jf.sender_ms = p.sender_ms;
                // Smooth bit kSmoothCaptureStamp (docs/real-partner-body.md section 3.5): the time Sam's pose
                // was computed (the same QPC clock as `now`, up to a frame earlier), so the receiver's
                // interpolation spaces the poses as they really were, whatever the sender's frame rate.
                if ((realbody::SmoothMask() & realbody::kSmoothCaptureStamp) && capture_ms > 0 && capture_ms <= now && now - capture_ms < 250)
                    jf.sender_ms = static_cast<std::uint32_t>(static_cast<std::uint64_t>(capture_ms));
                jf.body_id = realbody::LookBodyId(g_own_body, g_local);  // her black suit in the private room (section 34)
                // Our own Sam's outfit rides along (8 bytes; docs/real-partner-body.md section 9): a
                // receiver showing us as Sam dresses its body like our Sam.
                jf.flags = static_cast<std::uint8_t>(jf.flags & ~wire::kJointsOutfit);
                if (realbody::ReadOutfit(g_local, &jf.outfit)) jf.flags |= wire::kJointsOutfit;
                // Our own Sam's facial clip (2 bytes; section 36): the partner body plays it on its face.
                jf.flags = static_cast<std::uint8_t>(jf.flags & ~wire::kJointsFace);
                if (facesync::OwnFace(reinterpret_cast<std::uintptr_t>(g_local), &jf.face)) jf.flags |= wire::kJointsFace;
            }
            // Partner sounds (STA1, partner_sound.hpp, sound_sync=1): our sound events since the last send ride in the
            // same datagram (same sender_ms axis as the pose); in echo they go through encode / decode back to us.
            static wire::SoundFrame sf, sback;
            const bool have_sound = psound::TakeSend(g_local, now, &sf);
            if (g_net) {
                if (have_joints) link::SendPoseAndJoints(p, jf, have_sound ? &sf : nullptr);
                else link::SendPose(p, have_sound ? &sf : nullptr);
            } else if (g_ring.Push(p.sender_ms, now, p.x + g_cfg.echo_offset_m, p.y, p.z, p.rot)) {
                g_last_remote_ms = now;
                g_rx_pos[0] = p.x + g_cfg.echo_offset_m, g_rx_pos[1] = p.y, g_rx_pos[2] = p.z;
                g_rx_in_vehicle = (p.flags & wire::kPoseInVehicle) != 0;
                g_rx_flags = p.flags;
            }
            if (!g_net && have_sound) {
                std::uint8_t buf[wire::kMaxSoundFrameSize];
                const std::size_t n = wire::EncodeSound(sf, buf, sizeof(buf));
                if (n && wire::DecodeSound(buf, n, sback)) psound::OnFrame(sback, g_ring.Unwrapped(sback.sender_ms), now);
            }
            // Our vehicle (STM1, docs/real-partner-body.md section 22, vehicle_sync): in a datagram of its own, on the
            // pose's sender_ms axis; in echo through encode / decode back to us, moved like the echoed pose.
            {
                static wire::VehicleFrame vf, vback;
                if (vsync::TakeSend(g_local, g_local_wt, p.sender_ms, p.seq, now, &vf)) {
                    if (g_net) {
                        link::SendVehicle(vf);
                    } else {
                        std::uint8_t buf[wire::kMaxVehicleFrameSize];
                        const std::size_t n = wire::EncodeVehicle(vf, buf, sizeof(buf));
                        if (n && wire::DecodeVehicle(buf, n, vback)) {
                            vback.x += g_cfg.echo_offset_m;
                            vsync::OnFrame(vback, g_ring.Unwrapped(vback.sender_ms), now);
                        }
                    }
                }
                // its look (STV1 item 255, section 22.7): after a new link session again at once
                static wire::GearLook vl, vlback;
                if (g_net) {
                    static std::uint32_t vlook_gen = 0;
                    const std::uint32_t lg = link::Generation();
                    if (lg != vlook_gen) {
                        vlook_gen = lg;
                        vsync::MarkLookUnsent();
                    }
                }
                if (vsync::TakeSendLook(now, &vl)) {
                    if (g_net) {
                        if (!link::SendGearLook(vl)) vsync::MarkLookUnsent();  // not connected yet: again later
                    } else {
                        std::uint8_t buf[wire::kMaxGearLookFrameSize];
                        const std::size_t n = wire::EncodeGearLook(vl, buf, sizeof(buf));
                        if (n && wire::DecodeGearLook(buf, n, vlback)) vsync::OnLook(vlback);  // echo: our own vehicle's look
                    }
                }
                // The real shared vehicles (vehicle_sync=5; docs/real-partner-body.md section 23.11): STD1 frames for the vehicle
                // our Sam drives (the inputs the game feeds it) and the vehicles we own that nobody drives, on the same axis.
                if (g_net) {
                    static std::uint8_t dbuf[8][vwire::kMaxDriveFrameSize];
                    std::size_t dsz[8] = {};
                    const unsigned nd = vdrive::TakeSend(g_local, p.sender_ms, now, dbuf, dsz, 8);
                    for (unsigned i = 0; i < nd; ++i) link::SendDrive(dbuf[i], dsz[i]);
                }
            }
            if (!g_net && have_joints) {
                std::uint8_t buf[wire::kMaxJointFrameSize];
                const std::size_t n = wire::EncodeJoints(jf, buf, sizeof(buf));
                if (n && wire::DecodeJoints(buf, n, back)) {
                    TakeJointFrame(back, g_ring.Unwrapped(back.sender_ms));  // echo: our own body_id is the "peer's"
                }
                static bool logged = false;
                if (!logged && n) {
                    logged = true;
                    Log("[sync] joint frames (STJ1): %zu bytes each, %u joints, body_id %u; echoed through encode/decode", n,
                        static_cast<unsigned>(jf.joint_count), static_cast<unsigned>(jf.body_id));
                }
            }
            // Gear (STC1, docs/real-partner-body.md section 12): our backpack and cargo, when the layout
            // changes and about once a second. Only a game with the pose hook (a real-body game) sends it.
            static wire::GearFrame gf, gback;
            if (realbody::HookInstalled() && gear::TakeSend(now, &gf)) {
                static std::uint32_t gear_seq = 0;
                gf.seq = ++gear_seq;
                if (g_net) {
                    if (!link::SendGear(gf)) {
                        static unsigned unsent = 0;
                        std::uint8_t probe[wire::kMaxGearFrameSize];
                        if (!wire::EncodeGear(gf, probe, sizeof(probe)) && ++unsent <= 10)
                            Log("[gear] our gear frame (%u items) does not encode: not sent", static_cast<unsigned>(gf.count));
                    }
                } else {
                    std::uint8_t buf[wire::kMaxGearFrameSize];
                    const std::size_t n = wire::EncodeGear(gf, buf, sizeof(buf));
                    if (n && wire::DecodeGear(buf, n, gback)) gear::SetPartnerGear(&gback);  // echo: our own gear
                }
            }
            // Our gear resource table (STR1): the entity items' resources by id.
            static wire::GearRes gr, grback;
            if (g_net) {
                // a new link session: the partner's table is empty again (it clears it), every entry again
                static std::uint32_t res_gen = 0;
                const std::uint32_t lg = link::Generation();
                if (lg != res_gen) {
                    res_gen = lg;
                    gear::MarkResUnsent(-1);
                }
            }
            if (realbody::HookInstalled() && gear::TakeSendRes(now, &gr)) {
                if (g_net) {
                    if (!link::SendGearRes(gr)) gear::MarkResUnsent(gr.id);  // not connected yet: again later
                } else {
                    std::uint8_t buf[wire::kMaxGearResFrameSize];
                    const std::size_t n = wire::EncodeGearRes(gr, buf, sizeof(buf));
                    if (n && wire::DecodeGearRes(buf, n, grback)) gear::SetPartnerRes(grback);  // echo: our own table
                }
            }
            // Our gear's looks (STV1) and the backpack's pose (STK1), docs/real-partner-body.md section 13.
            if (g_net) {
                static std::uint32_t look_gen = 0;
                const std::uint32_t lg = link::Generation();
                if (lg != look_gen) {
                    look_gen = lg;
                    gear::MarkLooksUnsent();
                }
            }
            static wire::GearLook gl, glback;
            for (int k = 0; k < 4 && realbody::HookInstalled() && gear::TakeSendLook(now, &gl); ++k) {
                if (g_net) {
                    if (!link::SendGearLook(gl)) {
                        gear::MarkLooksUnsent();  // not connected yet: again later
                        break;
                    }
                } else {
                    std::uint8_t buf[wire::kMaxGearLookFrameSize];
                    const std::size_t n = wire::EncodeGearLook(gl, buf, sizeof(buf));
                    if (n && wire::DecodeGearLook(buf, n, glback)) gear::SetPartnerLook(glback);  // echo: our own looks
                }
            }
            static wire::GearPose gp, gpback;
            if (realbody::HookInstalled() && gear::TakeSendPose(now, &gp)) {
                if (g_net) {
                    link::SendGearPose(gp);
                } else {
                    std::uint8_t buf[wire::kMaxGearPoseFrameSize];
                    const std::size_t n = wire::EncodeGearPose(gp, buf, sizeof(buf));
                    if (n && wire::DecodeGearPose(buf, n, gpback)) gear::SetPartnerPose(gpback);  // echo: our own pose
                }
            }
        }
    }

    // ---- receive
    if (g_net) {
        wire::Pose p;
        std::uint64_t recv = 0;
        std::uint32_t gen = 0;
        // A new link session (any queue may show it first: an STR1 entry or a gear frame can arrive
        // before the session's first pose and must not be dropped as "from before the change").
        auto new_session = [&](std::uint32_t gen) {
            if (gen != g_generation) {
                g_generation = gen;
                g_ring.Reset();
                realbody::ResetFrames();
                g_peer_body.Reset();  // a new session may be another player with another character
                realbody::SetPartnerOutfit(nullptr);
                facesync::SetPartnerFace(nullptr);
                gear::SetPartnerGear(nullptr);
                g_first_joints_wait_ms = 0;
                pvoice::NewSession();
                psound::NewSession();
                vsync::NewSession();
                vdrive::NewSession();
                texshare::NewSession(gen);  // the host forgets the previous guest's textures look
                Log("[sync] new link session (#%u): interpolation reset", gen);
                if (BodyPresent()) RemoveBody("new link session");  // session end of the previous partner
            }
        };
        while (link::PopPose(p, recv, gen)) {
            new_session(gen);
            if (!(p.flags & wire::kPoseValid)) continue;
            g_rx_pos[0] = p.x, g_rx_pos[1] = p.y, g_rx_pos[2] = p.z;
            g_rx_in_vehicle = (p.flags & wire::kPoseInVehicle) != 0;
            g_rx_flags = p.flags;
            if (g_ring.Push(p.sender_ms, static_cast<double>(recv), p.x, p.y, p.z, p.rot))
                g_last_remote_ms = static_cast<double>(recv);
        }
        static wire::JointFrame jf;
        while (link::PopJoints(jf, recv, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;  // a frame from before the session change
            TakeJointFrame(jf, g_ring.Unwrapped(jf.sender_ms));
        }
        static wire::GearFrame gf;
        while (link::PopGear(gf, recv, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;  // a frame from before the session change
            gear::SetPartnerGear(&gf);
        }
        static wire::GearRes gr;
        while (link::PopGearRes(gr, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;
            gear::SetPartnerRes(gr);
        }
        static wire::GearLook gl;
        while (link::PopGearLook(gl, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;
            if (gl.item == wire::kLookVehicleItem) vsync::OnLook(gl);  // the partner vehicle's look (section 22.7)
            else gear::SetPartnerLook(gl);
        }
        static wire::GearPose gp;
        while (link::PopGearPose(gp, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;
            gear::SetPartnerPose(gp);
        }
        wire::Speech sp;
        while (link::PopSpeech(sp, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;
            pvoice::OnSpeech(sp, now);  // played in pvoice::Tick below, with this tick's talking entity
        }
        static wire::SoundFrame snd;
        while (link::PopSound(snd, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;
            psound::OnFrame(snd, g_ring.Unwrapped(snd.sender_ms), now);  // scheduled at the pose play time (psound::Tick)
        }
        static wire::VehicleFrame vf;
        while (link::PopVehicle(vf, gen)) {
            if (gen > g_generation) new_session(gen);
            if (gen != g_generation) continue;
            vsync::OnFrame(vf, g_ring.Unwrapped(vf.sender_ms), now);  // played at the pose play time (vsync::Tick)
        }
        {
            // the real shared vehicles' driven / parked frames (STD1): the strict decoder here, played at the pose play time
            std::uint8_t dbuf[vwire::kMaxDriveFrameSize];
            std::size_t dn = 0;
            static std::uint32_t bad_drive = 0;
            while (link::PopDrive(dbuf, sizeof(dbuf), dn, gen)) {
                if (gen > g_generation) new_session(gen);
                if (gen != g_generation) continue;
                vwire::DriveFrame df;
                if (vwire::DecodeDrive(dbuf, dn, df)) vdrive::OnFrame(df, g_ring.Unwrapped(df.sender_ms), now);
                else if (++bad_drive <= 10) Log("[sync] dropped an STD1 frame (%zu bytes): not valid", dn);
            }
        }
    }
    g_ring.Settle();

    // ---- partner body
    TeardownTriggers(now);
    DriveHolo(now);
    DriveReal(now);
    FollowPeerCharacter(now);
    ReturnToReal(now);
    const double delay = g_net ? static_cast<double>(g_cfg.interp_delay_ms) : g_cfg.echo_delay_s * 1000.0;
    const bool remote_fresh = g_last_remote_ms > 0 && now - g_last_remote_ms < 10000;
    if ((BodyPresent() || g_voice.e) && !remote_fresh) RemoveBody("partner silent for 10 s");
    // The first-joint wait counts only while positions arrive over a live link (TrySpawn runs).
    if (!remote_fresh || (g_net && g_link_down_since_ms > 0)) g_first_joints_wait_ms = 0;

    double pos[3] = {};
    wire::Quat q;
    const bool echo_ready = g_net || now - g_first_tick_ms > delay;
    psound::Frame sndf;  // partner sounds: where the partner is this tick and the pose play time
    vsync::TickIn vin;   // the partner's vehicle: the same play time as the body
    if (g_local_valid && remote_fresh && echo_ready && g_ring.Sample(now, delay, pos, q)) {
        WorldTransform target{};
        ComposeTransform(pos, q, target);
        if (g_pin) target = g_pin_wt;  // collision tests: the body stands still (its last pose held)
        // The real shared vehicles (section 23.15): while the partner drives one of our real vehicles here, its body sits at
        // that vehicle's transform times the partner's seat (the vehicle moves by its own physics; the body never drifts off it).
        // Riding together (section 24): the partner rides OUR vehicle as its passenger: its body sits at our vehicle times the seat.
        else if (!ride::PassengerTarget(&target)) vdrive::SeatTarget(g_ring.PlayTime(now, delay), &target);
        vin.have_play = true, vin.play_ms = g_ring.PlayTime(now, delay);
        vin.have_body = true, vin.body = target;
        sndf.have_root = true, sndf.root = target;
        sndf.have_play = true, sndf.play_ms = g_ring.PlayTime(now, delay);
        g_last_target[0] = pos[0], g_last_target[1] = pos[1], g_last_target[2] = pos[2];
        g_last_target_valid = true;
        if (!BodyPresent()) {
            TrySpawn(target, now);
        } else if (RealOurs()) {
            if (realbody::GetPhase() == realbody::Phase::live) {
                if (!realbody::Move(target)) {
                    Log("[sync] SetWorldTransform failed on the real body; removing it");
                    RemoveBody("set transform fault");
                    g_next_spawn_ms = now + 5000;
                } else if (now - g_last_remote_ms < 1000 && realbody::StarvedOfFrames(3000) && PeerMovedSinceJoints()) {
                    // Transforms arrive and move but no joint frames: the body would glide in a frozen
                    // pose, so the next body in the chain (the hologram) takes over until joints return.
                    StarvedFallBack("the partner sends positions but no joint frames (STJ1)", now);
                } else if (!g_pin) {
                    realbody::SamplePublish(g_ring.PlayTime(now, delay));  // joints on the transform's timeline
                }
            }
        } else if (holo::GetPhase() == holo::Phase::live) {
            if (!holo::Move(target)) {
                Log("[sync] SetWorldTransform failed on the hologram; removing it");
                RemoveBody("set transform fault");
                g_next_spawn_ms = now + 5000;
            }
        } else if (g_slot.e && !game::TrySetTransform(g_slot.e, target)) {
            Log("[sync] SetWorldTransform faulted on the partner; forgetting it");
            SlotForget(g_slot, "set transform fault");
            g_remote_state = "none";
            g_next_spawn_ms = now + 5000;
        }
        WorldTransform voice_at = target;
        if (g_cfg.voice_sync) pvoice::EmitterTransform(BodyEntity(), target, &voice_at);  // the partner's head
        if (g_voice.e && !game::TrySetTransform(g_voice.e, voice_at)) SlotForget(g_voice, "set transform fault");
        DriveVoice(voice_at, now);
    }

    // The partner's vehicle (vehicle_sync.hpp): its copy placed at the same play time as the body, under it.
    // asleep for under 1 s (a menu's first frames) keeps the copy, as the partner body and the gear props do
    vin.local_ok = g_local_valid && (g_local_active || (g_local_asleep_since_ms > 0 && now - g_local_asleep_since_ms < 1000));
    vin.partner_present = BodyPresent();
    vin.pose_age_ms = g_last_remote_ms > 0 ? now - g_last_remote_ms : 1e9;
    vin.local = g_local;
    vsync::Tick(vin, now);
    vsync::SenderTick(g_local, now);
    // The real shared vehicles (vehicle_sync=5 with host_save=1; vehicle_share.hpp): after vsync, whose partner vehicle place
    // tells the host's eviction refusal which vehicle the partner rides.
    {
        vshare::TickIn sin;
        sin.local = g_local;
        sin.local_ok = vin.local_ok;
        sin.have_partner = vin.have_body;
        sin.partner = vin.body;
        vshare::Tick(sin, now);
        // shared structures (world_share.hpp): the census, the join check, the mirrors
        {
            wshare::TickIn win;
            win.local = g_local;
            win.local_ok = vin.local_ok;
            win.have_partner = vin.have_body;
            win.partner = vin.body;
            wshare::Tick(win, now);
        }
        // remote driving, ownership and state on them (vehicle_drive.hpp): the partner's inputs at the body's play time
        vdrive::TickIn din;
        din.local = g_local;
        din.local_ok = vin.local_ok;
        din.have_play = vin.have_play;
        din.play_ms = vin.play_ms;
        din.have_partner = vin.have_body;
        din.partner = vin.body;
        vdrive::Tick(din, now);
        // the save guard (save_guard.hpp): watches the game's saves (before the ride, which reads its hold)
        saveguard::Tick(g_local, g_local_active, now);
        // riding together (ride_together.hpp): boarding, the carried passenger, the driver's side
        ride::TickIn rin;
        rin.local = g_local;
        rin.local_ok = g_local_valid;
        rin.local_active = g_local_active;
        rin.have_partner = vin.have_body;
        rin.partner = vin.body;
        rin.partner_rx_ms = g_last_remote_ms > 0 ? g_last_remote_ms : -1e18;
        rin.partner_flags = g_rx_flags;
        rin.have_play = vin.have_play;
        rin.play_ms = vin.play_ms;
        ride::Tick(rin, now);
        // vehicle hits (partner_tackle.hpp): the partner's vehicle hits our Sam (the game's own attack), the crash hit (D3-3)
        vhit::TickIn hin;
        hin.local = g_local;
        hin.local_ok = g_local_valid;
        hin.local_active = g_local_active;
        vhit::Tick(hin, now);
        // the players' shots (shot_sync.hpp; docs/enemy-sync.md P3a): ours captured and sent, the partner's replayed at the
        // body's play time from its gun
        shots::TickIn shin;
        shin.local = g_local;
        shin.local_ok = g_local_valid && g_local_active;
        shin.body = BodyEntity();
        shin.have_play = vin.have_play;
        shin.play_ms = vin.play_ms;
        shin.net = g_net;
        shin.role = static_cast<std::uint8_t>(g_cfg.role == Role::host ? 1 : g_cfg.role == Role::guest ? 2 : 0);
        shots::Tick(shin, now);
        esync::Tick(g_local, g_local_valid, now);  // the enemy freeze probe (hidden runs; idle otherwise)
    }

    // Player collision (partner_collision.hpp): only the partner slot's real body, never the
    // hologram or the self view's body.
    {
        Entity* real = RealOurs() && CurrentBody() == PartnerBody::real && realbody::GetPhase() == realbody::Phase::live
                           ? realbody::LiveEntity()
                           : nullptr;
        collision::Tick(g_local, g_local_active && g_local_valid, real, g_rx_in_vehicle, g_last_remote_ms > 0 ? now - g_last_remote_ms : 1e9,
                        now);
    }

    // Gear (partner_gear.hpp): our gear read, the partner's and the self view's props kept on their bodies.
    gear::Tick(g_local, g_local_active && g_local_valid, now);

    // Name tags (nametags.hpp): the partner's name above their head, our own name to the link.
    nametags::Tick(g_local, g_local_active && g_local_valid, BodyEntity(), remote_fresh, now);

    // Partner voice (partner_voice.hpp, voice_sync=1): our talks out (STS1), the partner's played on the talking entity.
    {
        pvoice::Frame vf;
        vf.local = g_local;
        vf.local_ok = g_local_active && g_local_valid;
        vf.body = BodyEntity();
        vf.emitter = g_voice.e ? g_voice.e : (CurrentBody() == PartnerBody::echo ? vf.body : nullptr);
        vf.net = g_net;
        pvoice::Tick(vf, now);
    }

    // Partner sounds (partner_sound.hpp, sound_sync=1): the partner's sound events posted at the pose play time on our
    // emitters at the body's joints; our own drained (they went out with the pose above).
    sndf.local = g_local;
    sndf.local_ok = g_local_active && g_local_valid;
    sndf.body = BodyEntity();
    sndf.net = g_net;
    psound::Tick(sndf, now);


    // One-shot diagnostics: did the partner stay where we put it, and is Sam fine?
    if (g_proxy_check_ms > 0 && now >= g_proxy_check_ms) {
        g_proxy_check_ms = 0;
        if (Entity* body = BodyEntity()) {
            WorldTransform pw{};
            game::TryGetTransform(body, &pw);
            Log("[sync] partner +1s (%s): flags=0x%llX at (%.2f, %.2f, %.2f) target (%.2f, %.2f, %.2f); Sam flags=0x%llX",
                PartnerBodyName(CurrentBody()), static_cast<unsigned long long>(Flags(body)), pw.pos.x, pw.pos.y, pw.pos.z,
                g_last_target[0], g_last_target[1], g_last_target[2], static_cast<unsigned long long>(Flags(g_local)));
            if (!holo::Describe("partner +1s")) partner::DescribeEntity(body, "partner +1s");
        }
    }

    if (g_cfg.verbose && now >= g_next_trace_ms) {
        g_next_trace_ms = now + 2000;
        Entity* body = BodyEntity();
        Log("[sync] trace local=(%.2f, %.2f, %.2f) awake=%d flags=0x%llX speed=%.2f partner=%p (%s/%s) partner_flags=0x%llX "
            "state=%s",
            g_local_wt.pos.x, g_local_wt.pos.y, g_local_wt.pos.z, g_local_active ? 1 : 0,
            static_cast<unsigned long long>(g_local ? Flags(g_local) : 0), g_speed, body, PartnerBodyName(CurrentBody()),
            holo::PhaseName(), static_cast<unsigned long long>(body ? Flags(body) : 0), g_remote_state);
    }

    LogTraffic(now);
    Hotkeys(now);
    PublishStatus(now);
    audioprobe::Tick(g_local, now);  // audio_probe=1 only: drains the probe rings (docs/audio-research.md section 9)
    selfvoice::Tick(g_local, now);   // self_voice muting only: publishes the local player and the menu figure, logs counts
    coop::Tick(now);     // host_save=1 only: the host-held save session (docs/coop-session.md)
    texshare::Tick(now, coop::WorldReady(nullptr) || (!coop::Active() && g_local_valid));  // Fragile's edited textures over the link (texture_share.hpp)
    if (coop::PlainNoticeWanted()) {   // host_save=0 for a host / guest: the features it costs, on screen
        char off[200];
        FeaturesOff(off, sizeof(off));
        coop::NoticeTick(off, g_local_valid, now);
    }
    debug::OnTick(now);  // debug-menu actions + snapshot (game thread)
}

// ------------------------------------------------------------------ debug menu
namespace {
template <std::size_t N>
void CopyText(char (&dst)[N], const char* src) {
    std::strncpy(dst, src ? src : "", N - 1);
    dst[N - 1] = 0;
}
}  // namespace

void FillDebugSnapshot(debug::Snapshot& s) noexcept {
    const double now = link::NowMsPrecise();
    s.net = g_net;
    if (g_net) {
        const link::Info li = link::GetInfo();
        CopyText(s.link_state, li.state);
        CopyText(s.transport, li.transport);
        CopyText(s.route, li.route);
        CopyText(s.peer, li.peer);
        CopyText(s.reason, li.reason);
        s.rtt_ms = li.rtt_ms;
        s.rx = li.rx_frames;
        s.tx = li.tx_frames;
    } else {
        CopyText(s.link_state, g_cfg.role == Role::solo ? "solo (no network)" : "echo (local)");
    }
    s.local_present = g_local != nullptr;
    s.local_valid = g_local_valid;
    s.local_awake = g_local_active;
    s.local_entity = reinterpret_cast<std::uintptr_t>(g_local);
    s.local_flags = g_local ? Flags(g_local) : 0;
    if (g_local_valid) {
        s.local_pos[0] = g_local_wt.pos.x, s.local_pos[1] = g_local_wt.pos.y, s.local_pos[2] = g_local_wt.pos.z;
        std::memcpy(s.local_rot, g_local_wt.rot.m, sizeof(s.local_rot));
    }
    s.speed = g_speed;

    Entity* body = BodyEntity();
    s.partner_entity = reinterpret_cast<std::uintptr_t>(body);
    s.partner_voice_entity =
        reinterpret_cast<std::uintptr_t>(g_voice.e ? g_voice.e : (CurrentBody() == PartnerBody::echo ? body : nullptr));
    CopyText(s.partner_state, g_remote_state);
    char src[sizeof(s.partner_source)] = {};
    if (CurrentBody() == PartnerBody::real && realbody::CurrentBody())
        std::snprintf(src, sizeof(src), "real %s", realbody::CurrentBody()->key);
    else if (CurrentBody() == PartnerBody::holo && holo::CurrentHash())
        std::snprintf(src, sizeof(src), "holo %08X", holo::CurrentHash());
    else if (CurrentBody() == PartnerBody::marker && g_slot.src.res)
        std::snprintf(src, sizeof(src), "marker #%d", g_slot.src.index);
    else
        std::snprintf(src, sizeof(src), "%s", PartnerBodyName(CurrentBody()));
    CopyText(s.partner_source, src);
    s.partner_flags = body ? Flags(body) : 0;
    if (body) {
        WorldTransform pw{};
        if (game::TryGetTransform(body, &pw)) {
            s.partner_pos_valid = true;
            s.partner_pos[0] = pw.pos.x, s.partner_pos[1] = pw.pos.y, s.partner_pos[2] = pw.pos.z;
        }
    }
    s.spawn_disabled = g_spawn_disabled;
    s.spawn_held = g_spawn_held;
    s.remote_fresh = g_last_remote_ms > 0 && now - g_last_remote_ms < 10000;
    s.remote_age_ms = g_last_remote_ms > 0 ? now - g_last_remote_ms : -1;

    s.echo_delay_s = g_cfg.echo_delay_s;
    s.echo_offset_m = g_cfg.echo_offset_m;
    s.interp_delay_ms = g_cfg.interp_delay_ms;
    s.send_hz = g_cfg.send_hz;
    s.verbose = g_cfg.verbose;
    s.tick_hz = g_tick_hz;
    s.ticks = g_frames;
}

void DebugRespawnPartner() noexcept { RespawnPartnerNow(link::NowMsPrecise(), "debug menu"); }

bool DebugRemovePartner() noexcept {
    const bool had = BodyPresent();
    RemoveBody("debug menu");
    g_spawn_held = true;
    return had;
}

bool DebugSetPartnerVisible(bool visible) noexcept {
    if (holo::LiveEntity()) return holo::SetShown(visible);
    if (!SlotAlive(g_slot)) return false;
    return partner::SetVisible(g_slot.e, visible);
}

void DebugSetEchoTuning(float delay_s, float offset_m) noexcept {
    if (delay_s < 0.1f) delay_s = 0.1f;
    if (delay_s > 15.0f) delay_s = 15.0f;  // the pose ring holds > 15 s at 30 Hz
    if (offset_m < -50.0f) offset_m = -50.0f;
    if (offset_m > 50.0f) offset_m = 50.0f;
    g_cfg.echo_delay_s = delay_s;
    g_cfg.echo_offset_m = offset_m;
}

void DebugSetVerbose(bool on) noexcept { g_cfg.verbose = on; }

void DebugLogPositions() noexcept { LogPositionsNow(); }

bool DebugLogPartner() noexcept {
    if (holo::Describe("debug menu")) return true;
    if (Entity* r = RealOurs() ? realbody::LiveEntity() : nullptr) {
        partner::DescribeEntity(r, "debug menu (real body)");
        return true;
    }
    if (!SlotAlive(g_slot)) return false;
    partner::DescribeEntity(g_slot.e, "debug menu");
    return true;
}

bool DebugPinPartner(double dist_m, double dz_m, char* msg, std::size_t cap, bool away) noexcept {
    if (!(dist_m > 0)) {
        g_pin = false;
        Log("[sync] partner unpinned: it follows the stream again");
        if (msg && cap) std::snprintf(msg, cap, "partner unpinned");
        return true;
    }
    // Along the camera's forward (where the pad's forward walks Sam), else his last walking direction.
    double dir[2] = {g_move_dir[0], g_move_dir[1]};
    const bool camera = nametags::CameraForward(g_local, dir);
    const double dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    if (!g_local_valid || dl < 0.5 || dist_m > 50 || !(dz_m >= -10 && dz_m <= 10)) {
        if (msg && cap) std::snprintf(msg, cap, "pin: no local player, no direction, more than 50 m, or a height offset outside -10..10 m");
        return false;
    }
    WorldTransform w = g_local_wt;
    w.pos.x += dir[0] * dist_m;
    w.pos.y += dir[1] * dist_m;
    w.pos.z += dz_m;
    // Turned 180 degrees about the up axis (Sam is upright): it faces back towards him. `away`: Sam's
    // own heading (its back to him and the camera: back shots).
    if (!away)
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c) w.rot.m[r][c] = -w.rot.m[r][c];
    g_pin_wt = w;
    g_pin = true;
    Log("[sync] partner pinned %.2f m ahead of Sam (%+.2f m height) along the %s (%.3f, %.3f), %s: at (%.3f, %.3f, %.3f)", dist_m, dz_m,
        camera ? "camera's forward" : "last walking direction", dir[0], dir[1], away ? "its back to him" : "facing him", w.pos.x, w.pos.y, w.pos.z);
    if (msg && cap) std::snprintf(msg, cap, "partner pinned %.2f m ahead", dist_m);
    return true;
}

bool DebugPartnerPosition(double (&out)[3]) noexcept {
    if (Entity* body = BodyEntity()) {
        WorldTransform pw{};
        if (game::TryGetTransform(body, &pw)) {
            out[0] = pw.pos.x, out[1] = pw.pos.y, out[2] = pw.pos.z;
            return true;
        }
    }
    if (g_last_target_valid) {
        out[0] = g_last_target[0], out[1] = g_last_target[1], out[2] = g_last_target[2];
        return true;
    }
    return false;
}

}  // namespace st::sync
