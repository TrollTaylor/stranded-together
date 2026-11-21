// Shared real vehicles (design milestone M2; docs/real-partner-body.md section 23): the pure part. The vehicle event
// messages on the link's reliable ordered channel 3 (one "SVM1" header, then one body per type), the snapshot's chunking
// and reassembly, and the decision rules of vehicle_share.cpp (binding a guest unit to the host's, the host's removal
// guard, the eviction refusal, the spawn request flags). No game memory, no link, no Windows: unit-tested in
// tests/vehicle_wire_tests.cpp.
//
// Identity (M2.2): a vehicle is the host's unit. Its key is VKey {the host's game-actor id (unit+0x08 =
// (actorType << 13) | slot; actor types 0x20 roadster, 0x10 truck, 0x0F trike), its kind (1 roadster, 2 truck, 3 trike:
// EVehicleType, the pool index), its 16-byte placement GUID (unit+0x188; zero for player-left and runtime vehicles)}.
// The host's ids are canonical; the guest keeps host id -> its own unit.
//
// Every decoder is strict: exact sizes, the magic, version 1, known types, zero reserved bytes, kinds consistent with
// the actor id, finite numbers in range, printable names. A message that fails is dropped whole (and counted).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace st::vwire {

// ---------------------------------------------------------------- constants
inline constexpr std::uint8_t kChannel = 3;              // the link's reliable ordered channel for vehicle events
inline constexpr std::size_t kMaxMessage = 1024;         // link_protocol.hpp kMaxReliableMessage
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 8;             // "SVM1" | u8 type | u8 version | u16 reserved (0)
inline constexpr std::uint32_t kNoRider = 0x1FFFFF;       // unit+0x158 with nobody riding
inline constexpr std::size_t kEntrySize = 0x74;           // the game's persisted vehicle entry (save blob section entry)
inline constexpr std::size_t kNameChars = 32;

enum class Kind : std::uint8_t { none = 0, roadster = 1, truck = 2, trike = 3 };
const char* KindName(std::uint8_t kind) noexcept;
// Actor type in the id's bits 13+ for a kind (0 for none).
std::uint32_t ActorTypeOfKind(std::uint8_t kind) noexcept;
// The kind an actor id belongs to (0 for none).
std::uint8_t KindOfActorId(std::uint32_t id) noexcept;

// Message types. 1..15 vehicle_share (this stage), 16..31 ownership (VOWN: CLAIM / GRANT / DENY / RELEASE), 32..63 the
// drive module (remote driving, parked state). Types 16..63 are not decoded here: vehicle_share hands them to the
// handler registered for that type (vehicle_share.hpp RegisterHandler).
enum class MsgType : std::uint8_t {
    snap_req = 1,    // guest -> host: send me your vehicles (after every guest load, and on demand)
    snap = 2,        // host -> guest: one chunk of the host's vehicle table
    add = 3,         // host -> guest: a vehicle appeared in the host's world (its own spawn, a guest's request, a re-announce)
    rem = 4,         // either way: a vehicle was removed in the sender's world
    req = 5,         // guest -> host: the guest's game asked for a vehicle (dropped at the guest's enqueue)
    req_result = 6,  // host -> guest: what became of a request
    need = 7,        // guest -> host: re-announce this vehicle (an ADD with its persisted entry): the guest must mirror it
    first_external = 16,
    last_external = 63,
};
const char* MsgTypeName(std::uint8_t type) noexcept;
inline bool IsExternalType(std::uint8_t t) noexcept { return t >= 16 && t <= 63; }

// ---------------------------------------------------------------- identity
struct VKey {
    std::uint32_t id = 0;
    std::uint8_t kind = 0;
    std::uint8_t guid[16] = {};
    bool HasGuid() const noexcept {
        for (std::uint8_t b : guid)
            if (b) return true;
        return false;
    }
    bool SameGuid(const std::uint8_t (&g)[16]) const noexcept { return std::memcmp(guid, g, 16) == 0; }
    bool operator==(const VKey& o) const noexcept { return id == o.id && kind == o.kind && std::memcmp(guid, o.guid, 16) == 0; }
    bool operator!=(const VKey& o) const noexcept { return !(*this == o); }
};
// A key that decodes: kind 1..3 and the id's actor type is that kind's.
bool KeyValid(const VKey& k) noexcept;

// ---------------------------------------------------------------- the host's table (VSNAP entries)
inline constexpr std::uint8_t kEntEntity = 1u;    // the unit has a spawned entity (its actor is in the world)
inline constexpr std::uint8_t kEntXf = 2u;        // pos / rot are valid (the entity's, else the unit's own)
inline constexpr std::uint8_t kEntRemoved = 4u;   // removed but remembered (unit+0x19C & 0x40)
inline constexpr std::uint8_t kEntReserved = 8u;  // the "called" unit (unit+0x19E & 8)
inline constexpr std::uint8_t kEntRidden = 16u;   // somebody rides it (unit+0x158 != 0x1FFFFF)
inline constexpr std::uint8_t kEntAllBits = 31u;

struct SnapEntry {
    VKey key;
    std::uint8_t state = 0;        // unit+0x24 (0 none, 2 spawned)
    std::uint8_t eflags = 0;       // kEnt*
    double pos[3] = {};
    float rot[9] = {};             // the 3x3 rows of game::WorldTransform::rot (right, forward, up)
    std::uint32_t flags = 0;       // unit+0x198
    std::uint8_t flag_bytes[4] = {};  // unit+0x19C..0x19F
    std::uint16_t area = 0;        // unit+0x1A4
    std::uint16_t rec2 = 0;        // unit+0x1A2 (the spawn record's +2)
    float life = 0;                // the entity's life (+0x6E8), else unit+0x150
    float battery = 0;             // unit+0x154
    std::uint32_t rider = kNoRider;  // unit+0x158
    bool Has(std::uint8_t bit) const noexcept { return (eflags & bit) != 0; }
};
inline constexpr std::size_t kSnapEntrySize = 108;
inline constexpr std::size_t kSnapChunkHeader = 20;
inline constexpr std::size_t kSnapPerChunk = (kMaxMessage - kHeaderSize - kSnapChunkHeader) / kSnapEntrySize;  // 9
inline constexpr std::size_t kSnapMaxChunks = 32;
inline constexpr std::size_t kSnapMaxEntries = 256;

struct SnapChunk {
    std::uint32_t snap_id = 0;           // one snapshot: every chunk carries it
    std::uint32_t req_id = 0;            // the SNAP_REQ it answers (0: sent unasked)
    std::uint32_t host_load_counter = 0; // the host's world (coop_session's load counter)
    std::uint16_t host_area = 0;         // the host's current area
    std::uint8_t index = 0, count = 1;   // this chunk of `count`
    std::uint16_t total = 0;             // entries in the whole snapshot
    std::vector<SnapEntry> entries;      // at most kSnapPerChunk
};

struct SnapReq {
    std::uint32_t req_id = 0;
    std::uint32_t load_counter = 0;  // the guest's loaded host save (coop_session's)
    std::uint8_t reason = 0;         // 0 after a load, 1 periodic, 2 test / manual
};

// ADD: a vehicle the host has. `entry` is the game's own persisted entry for it (0x142F7CF20 on the host), which the
// guest's exact-id mirror feeds to the persistence loader's spawn path.
enum class AddSource : std::uint8_t { host_own = 0, guest_request = 1, reannounce = 2 };
struct Add {
    std::uint32_t token = 0;   // the guest's request token (guest_request), else 0
    AddSource source = AddSource::host_own;
    std::uint16_t rec0 = 0;    // the spawn record's +0 (pool | variant bits) for the guest's fallback spawn
    std::uint8_t p4 = 0;       // 0x142F830A0's 4th argument
    SnapEntry unit;
    bool has_entry = false;
    std::uint8_t entry[kEntrySize] = {};
};

enum class RemReason : std::uint8_t { gone = 0, removed_flag = 1, released = 2, destroyed = 3, test = 4 };
struct Rem {
    VKey key;
    RemReason reason = RemReason::gone;
    std::uint8_t from = 0;    // 0 host, 1 guest
    bool has_pos = false;
    double pos[3] = {};
    std::uint32_t world = 0;  // the sender's world (coop_session's load counter): the receiver drops one of another world
};

// dev: the guest's dev tools (dev_tools.cpp): the host serves it only when its own saves are sandboxed (dev_rules.hpp).
// A v0.18 host's decoder refuses it (reason > 2): nothing spawns.
enum class ReqReason : std::uint8_t { game = 0, garage = 1, test = 2, dev = 3 };
struct Req {
    std::uint32_t token = 0;
    std::uint8_t kind = 0;           // the pool the request went to
    std::int8_t garage_slot = -1;    // the parking manager's slot (0..8) when it came from 0x142D5CB20
    ReqReason reason = ReqReason::game;
    std::uint32_t req_flags = 0;     // the 0x68 request's +0x60
    std::uint16_t rec2 = 0;          // request +0x54
    std::uint64_t rec14 = 0;         // request +0x58
    double pos[3] = {};              // the transform asked for (for a garage slot: the one passed to 0x142D5CB20)
    float rot[9] = {};
    char name[kNameChars + 1] = {};  // the request's name (printable ASCII, "" for most)
};

enum class ReqResult : std::uint8_t { spawned = 0, refused = 1, failed = 2, timeout = 3, duplicate = 4 };
const char* ReqResultName(std::uint8_t r) noexcept;
struct Result {
    std::uint32_t token = 0;
    ReqResult result = ReqResult::refused;
    std::uint8_t path = 0;  // host: 1 garage slot, 2 spawn record
    VKey key;               // the spawned vehicle (spawned only)
};

struct Need {
    VKey key;
    std::uint8_t reason = 0;  // 0 mirror, 1 re-mirror after a guest removal
};

// ---------------------------------------------------------------- encode / decode
// Encoders return the size written (0: does not fit or invalid input). Decoders return false for anything not exactly
// a valid message of that type.
std::size_t EncodeSnapReq(const SnapReq& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeSnapReq(const std::uint8_t* in, std::size_t size, SnapReq& out) noexcept;
std::size_t EncodeSnapChunk(const SnapChunk& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeSnapChunk(const std::uint8_t* in, std::size_t size, SnapChunk& out) noexcept;
std::size_t EncodeAdd(const Add& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeAdd(const std::uint8_t* in, std::size_t size, Add& out) noexcept;
std::size_t EncodeRem(const Rem& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeRem(const std::uint8_t* in, std::size_t size, Rem& out) noexcept;
std::size_t EncodeReq(const Req& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeReq(const std::uint8_t* in, std::size_t size, Req& out) noexcept;
std::size_t EncodeResult(const Result& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeResult(const std::uint8_t* in, std::size_t size, Result& out) noexcept;
std::size_t EncodeNeed(const Need& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeNeed(const std::uint8_t* in, std::size_t size, Need& out) noexcept;
// The header of any channel-3 message (also the external types 16..63): magic, version 1, reserved 0. The body is the
// rest (`size - kHeaderSize` bytes from in + kHeaderSize).
bool PeekType(const std::uint8_t* in, std::size_t size, std::uint8_t* type) noexcept;
// Writes the header of an external message (types 16..63) for the drive module; returns kHeaderSize, 0 for a bad type.
std::size_t WriteHeader(std::uint8_t type, std::uint8_t* out, std::size_t cap) noexcept;

// The host's whole table as chunks (each one message). Empty: no chunk at all (an empty table is one chunk with no
// entries). More than kSnapMaxEntries entries: the rest is left out (the guest asks again later).
std::vector<std::vector<std::uint8_t>> EncodeSnapshot(std::uint32_t snap_id, std::uint32_t req_id, std::uint32_t load_counter, std::uint16_t area,
                                                      const std::vector<SnapEntry>& entries);

// The guest's reassembly of one snapshot. A chunk of a newer snapshot (another snap_id) starts over; a duplicate chunk,
// a chunk whose header disagrees with the snapshot's, or more entries than announced is rejected (and the snapshot
// starts over at its next chunk 0).
class SnapAssembler {
public:
    enum class Step : std::uint8_t { partial, complete, rejected };
    Step Add(const SnapChunk& c) noexcept;
    bool Complete() const noexcept { return complete_; }
    std::uint32_t SnapId() const noexcept { return snap_id_; }
    std::uint32_t ReqId() const noexcept { return req_id_; }
    std::uint32_t LoadCounter() const noexcept { return load_counter_; }
    std::uint16_t Area() const noexcept { return area_; }
    const std::vector<SnapEntry>& Entries() const noexcept { return entries_; }
    void Reset() noexcept;

private:
    bool active_ = false, complete_ = false;
    std::uint32_t snap_id_ = 0, req_id_ = 0, load_counter_ = 0;
    std::uint16_t area_ = 0, total_ = 0;
    std::uint8_t count_ = 0;
    std::uint32_t have_ = 0;  // bit per chunk
    std::vector<SnapEntry> entries_;
};

// ---------------------------------------------------------------- spawn request flags (0x142F830A0)
// 0x142F830A0 builds the 0x68 request's flags (+0x60) from the spawn record's +0 (u16) and its p4 / p6 arguments:
// bits 0..1 = record bits 4..5, bit 2 = p4, bits 4..9 = record bits 6..11, bit 12 = record bit 12, bit 13 = record bit 13,
// bit 14 = p6; bits 3, 10, 11 and 15+ are never set by it (0xC0A online requests set 1, 3, 10, 11).
std::uint32_t RequestFlagsFromRecord(std::uint16_t rec0, std::uint8_t p4, std::uint8_t p6) noexcept;
// The inverse for a request made by 0x142F830A0 (pool type = the pool the request went to, 1..3). False when the flags
// carry bits 0x142F830A0 never sets (an online request) or the pool type is bad.
bool RecordFromRequestFlags(std::uint32_t flags, std::uint8_t pool_type, std::uint16_t* rec0, std::uint8_t* p4, std::uint8_t* p6) noexcept;
inline constexpr std::uint32_t kReqOnline = 0x800;  // an online (other players') vehicle: dropped during a session (D2-3)

// Who may pass the enqueue (0x142F654E0) during a session.
enum class Gate : std::uint8_t {
    pass,        // the game's own request (host), the mod's own call, or the guest's load window
    drop_online, // flag 0x800 (D2-3), both games
    forward,     // guest: dropped and sent to the host as a REQ
    drop,        // guest: dropped, not forwarded (rate limit, no link, before the first snapshot)
};
struct GateIn {
    bool host = false;          // this game is the host
    bool mod_call = false;      // the mod's own spawn is running (thread-local flag)
    bool load_window = false;   // guest: its world is loading or settling (the save's own spawns replay as on the host)
    bool can_forward = false;   // guest: linked, in the world, snapshot bound, under the rate limit
    bool host_real = true;      // guest: the host's game shares real vehicles (it answered with a snapshot in this world and
                                // has not gone silent); false: the guest's own game spawns as without the sync (pass)
    std::uint32_t flags = 0;    // the request's +0x60
};
Gate DecideGate(const GateIn& in) noexcept;

// ---------------------------------------------------------------- binding (M2.2, guest)
// A guest unit as its pool poll saw it.
struct LocalUnit {
    std::uint32_t id = 0;
    std::uint8_t kind = 0;
    bool has_entity = false;
    bool has_pos = false;
    double pos[3] = {};
    std::uint8_t guid[16] = {};
    bool removed = false;              // unit+0x19C & 0x40
    bool ridden = false;               // unit+0x158 != 0x1FFFFF (the guest's Sam or an NPC)
    std::uint32_t bound_host_id = 0;   // an earlier binding to another host id (a fallback mirror's id mapping); 0 none
    std::uint32_t mirrored_for = 0;    // the mod mirrored the host's vehicle with this id into it (any id, the same one too):
                                       // it is that vehicle whatever its GUID (a mirror carries none); 0 none
    bool pending_mirror = false;       // a fallback mirror spawned it and it is not mapped yet: leave it alone
    bool HasGuid() const noexcept {
        for (std::uint8_t b : guid)
            if (b) return true;
        return false;
    }
};
enum class BindKind : std::uint8_t {
    bound,              // the same vehicle (same id / GUID, within the radius)
    bound_unverified,   // the same id and kind, a position missing on one side: bound, checked again later
    replace,            // the guest's unit with that id is another vehicle of that kind: re-placed onto the host's spot
    remove_then_mirror, // the guest's unit with that id is another vehicle: removed (0x2000), the host's one mirrored after
    remove_local,       // the host removed it (removed but remembered): the guest's counterpart is removed
    mirror,             // the guest has no unit for it: mirrored (M2.3)
    wait,               // not yet (inside the grace time, another area, the guest's unit is ridden)
    gone,               // removed on the host and absent here: nothing to do
};
const char* BindKindName(BindKind k) noexcept;
struct BindStep {
    int host = -1;    // index into the host entries
    int local = -1;   // index into the local units (-1 none)
    BindKind kind = BindKind::wait;
    double dist = -1; // metres between the two when both are known
};
enum class ExtraKind : std::uint8_t { remove, keep_ridden, keep_pending, already_removed };
struct ExtraStep {
    int local = -1;
    ExtraKind kind = ExtraKind::remove;
};
struct BindParams {
    double radius_m = 5.0;          // the same vehicle when within this (no GUID)
    bool mirrors_allowed = false;   // the grace time after the guest's load is over
    std::uint16_t current_area = 0; // the guest's area: only vehicles of this area are mirrored
    bool allow_replace = true;      // re-place a wrong same-kind unit (RequestTeleport) instead of remove + mirror
};
struct BindPlan {
    std::vector<BindStep> steps;   // one per host entry, in order
    std::vector<ExtraStep> extras; // guest units no host entry claims
    unsigned bound = 0, unverified = 0, replace = 0, remove_mirror = 0, remove_local = 0, mirror = 0, wait = 0, gone = 0;
    unsigned extra_remove = 0, extra_kept = 0;
};
BindPlan PlanBinding(const std::vector<SnapEntry>& host, const std::vector<LocalUnit>& local, const BindParams& p);

// ---------------------------------------------------------------- host rules
// The host's guard before it sets 0x2000 on its own unit for a removal it did not decide (M2.4, M2.11): the unit must be
// the key's vehicle (in use, the same id, kind and GUID, not removed already) and, when the unit has a position, the
// sender must have given one within the radius (a removal without a place is refused for a unit that has one).
struct HostUnitView {
    bool in_use = false;
    std::uint32_t id = 0;
    std::uint8_t kind = 0;
    std::uint8_t guid[16] = {};
    bool removed = false;
    bool has_pos = false;
    double pos[3] = {};
};
bool HostRemovalAllowed(const HostUnitView& unit, const VKey& key, bool has_pos, const double (&pos)[3], double radius_m = 5.0) noexcept;

// The host's eviction refusal (M2.5): a unit is shared-active while the partner rides it, was within 150 m of it in the
// last 60 s, or drove it in the last 10 min.
inline constexpr double kActiveNearM = 150.0, kActiveNearMs = 60000.0, kActiveDroveMs = 600000.0;
struct ActiveInfo {
    bool partner_riding = false;
    double last_near_ms = -1e18;   // the partner within kActiveNearM of it
    double last_drove_ms = -1e18;  // the partner drove it
};
bool SharedActive(const ActiveInfo& a, double now_ms) noexcept;

// Rate limit of the guest's forwarded requests: at most `burst` within `window_ms`.
class RateLimit {
public:
    RateLimit(unsigned burst, double window_ms) noexcept : burst_(burst > 8 ? 8 : burst), window_(window_ms) {}
    bool Allow(double now_ms) noexcept;
private:
    unsigned burst_;
    double window_;
    double times_[8] = {-1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18, -1e18};
    unsigned next_ = 0;
};

// Metres between two points.
double Dist(const double (&a)[3], const double (&b)[3]) noexcept;

// ================================================================ stage 2: remote driving and ownership (M2.6 - M2.8)
// The drive module (vehicle_drive.cpp) sends two kinds of frames per vehicle as unreliable link FRAME messages ("STD1", on
// the pose send tick and on the pose's sender_ms axis): DRIVE while the sender's Sam drives it (30 Hz, with the 48 input
// bytes the game feeds the vehicle), PARK for a vehicle the sender owns that nobody drives (1 Hz and at once on a change of
// 5 cm / 2 deg, within 300 m of either player). Ownership travels on channel 3 (VOWN, types 16..19 below).

// ---------------------------------------------------------------- STD1
inline constexpr std::uint8_t kDriveMagic[4] = {'S', 'T', 'D', '1'};
inline constexpr std::uint8_t kDriveVersion = 1;
inline constexpr std::size_t kDriveInputBytes = 48;       // +0x08..+0x37 of the input object [vehicle+0x528]
inline constexpr std::size_t kDriveParkSize = 124;        // the PARK frame
inline constexpr std::size_t kDriveFrameSize = 172;       // the DRIVE frame (PARK + the inputs)
inline constexpr std::size_t kMaxDriveFrameSize = kDriveFrameSize;
enum class DriveType : std::uint8_t { drive = 1, park = 2 };
inline constexpr std::uint8_t kDrvSeated = 1u;      // the sender's Sam holds the driver seat (DRIVE only, always set there)
inline constexpr std::uint8_t kDrvEngine = 2u;      // the engine runs (+0x564 bit 0 / the ride state)
inline constexpr std::uint8_t kDrvBroken = 4u;      // +0x560 & 0x2000 (broken)
inline constexpr std::uint8_t kDrvExploded = 8u;    // +0xBD8 & 0x100000 (the explosion latched by the ride-state machine)
inline constexpr std::uint8_t kDrvAtRest = 16u;     // under 0.2 m/s for 1 s on the sender
inline constexpr std::uint8_t kDrvBattery = 32u;    // `battery` carries the probed battery field (else 0)
inline constexpr std::uint8_t kDrvPhysVel = 64u;    // v0.21: vel / avel are the chassis body's physics velocities (world;
                                                    // avel all three axes), not a finite difference of the transform
inline constexpr std::uint8_t kDrvAllFlags = 127u;
struct DriveFrame {
    DriveType type = DriveType::park;
    std::uint8_t flags = 0;
    std::uint8_t ride_state = 0;   // +0x4CC (0..15)
    std::uint32_t seq = 0;
    std::uint32_t sender_ms = 0;   // the pose's axis
    std::uint32_t epoch = 0;       // the ownership epoch the sender owns it under (VOWN)
    VKey key;                      // the shared identity (the host's id)
    double pos[3] = {};
    float quat[4] = {0, 0, 0, 1};  // x, y, z, w of the rotation rows (right, forward, up)
    float vel[3] = {};             // m/s, measured by the sender
    float avel[3] = {};            // rad/s, measured by the sender
    float speed_kmh = 0;           // [[vehicle+0x330]+0xF0]
    float life = 0, life_max = 0;  // +0x6E8 / +0x6EC
    float battery = 0;             // +0x5A4 when kDrvBattery
    std::uint8_t input[kDriveInputBytes] = {};  // DRIVE only
    bool Has(std::uint8_t f) const noexcept { return (flags & f) != 0; }
};
// The 48 input bytes as the game's input object holds them: 11 floats (+0x08..+0x30), finite and |x| <= 4, then the bytes
// +0x34..+0x36 0 / 1 and +0x37 zero (the sender clears it).
bool InputsValid(const std::uint8_t (&in)[kDriveInputBytes]) noexcept;
std::size_t EncodeDrive(const DriveFrame& f, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: the magic, version 1, type 1 or 2 with its exact size, known flag bits (seated on DRIVE only and always there),
// ride state <= 15, a valid key, finite numbers in range (|pos| <= 1e7, a unit quaternion within 10 %, |vel| <= 200 m/s,
// |avel| <= 50 rad/s, |speed| <= 1000, 0 <= life <= max <= 1e6, battery 0..1e6 only with its flag, else 0) and valid inputs.
bool DecodeDrive(const std::uint8_t* in, std::size_t size, DriveFrame& out) noexcept;
// The frame's magic and size class (a quick test for the link's frame dispatch).
bool LooksLikeDrive(const std::uint8_t* in, std::size_t size) noexcept;

// ---------------------------------------------------------------- VOWN (channel 3 types 16..19)
enum class OwnType : std::uint8_t { claim = 16, grant = 17, deny = 18, release = 19 };
inline constexpr std::uint8_t kOwnerHost = 0, kOwnerGuest = 1;
// CLAIM reasons (guest -> host)
inline constexpr std::uint8_t kClaimDrive = 0, kClaimPush = 1;
// GRANT reasons (host -> guest: who owns it now and why)
inline constexpr std::uint8_t kGrantClaim = 0, kGrantHostTook = 1, kGrantHandback = 2, kGrantTimeout = 3;
// DENY reasons (kDenyNotThere: the host does not see the guest's Sam at the vehicle)
inline constexpr std::uint8_t kDenyHostDrives = 0, kDenyHostFirst = 1, kDenyBusy = 2, kDenyNotThere = 3;
// RELEASE reasons (the owner, either way)
inline constexpr std::uint8_t kReleaseGotOff = 0, kReleaseAtRest = 1;
inline constexpr std::size_t kOwnBodySize = 92;
struct Own {
    OwnType type = OwnType::claim;
    VKey key;
    std::uint8_t owner = kOwnerHost;  // GRANT / DENY: the owner now; CLAIM / RELEASE: the sender
    std::uint8_t reason = 0;
    std::uint32_t epoch = 0;          // GRANT / DENY: the new epoch; CLAIM: the one the sender knows; RELEASE: the owned one
    std::uint32_t sender_ms = 0;
    bool has_xf = false;              // RELEASE only: the final place and velocity
    double pos[3] = {};
    float quat[4] = {0, 0, 0, 1};
    float vel[3] = {};
    std::uint32_t world = 0;          // the sender's world (coop_session's load counter): another world's message is dropped
};
std::size_t EncodeOwn(const Own& m, std::uint8_t* out, std::size_t cap) noexcept;
// Strict: header (types 16..19), exact size, valid key, owner 0 / 1, the reason in its type's range, has_xf only on
// RELEASE, the transform finite (zeros without), CLAIM from the guest and GRANT / DENY naming an owner.
bool DecodeOwn(const std::uint8_t* in, std::size_t size, Own& out) noexcept;
const char* OwnTypeName(std::uint8_t t) noexcept;

// ---------------------------------------------------------------- ownership rules (M2.7)
// One vehicle's owner as both games know it. The host arbitrates; each change bumps the epoch.
struct OwnState {
    std::uint8_t owner = kOwnerHost;
    std::uint32_t epoch = 0;
};
struct OwnStep {
    OwnState next;
    bool send = false;           // the host answers / announces with `type`
    OwnType type = OwnType::grant;
    std::uint8_t reason = 0;
};
// Host: the guest claims it. `host_sam_on_it`: the host's Sam holds (or is getting into) the driver seat; `host_drove_ms_ago`:
// since the host's Sam last held it (< 0 never). The host wins every tie (its Sam on it, or it got on within 300 ms).
OwnStep HostOnClaim(const OwnState& s, std::uint8_t claim_reason, bool host_sam_on_it, double host_drove_ms_ago) noexcept;
// Host: its own Sam got on (or pushes it): a guest owner loses it.
OwnStep HostOnHostTakes(const OwnState& s) noexcept;
// Host: the guest released it. At rest (or `got off` at rest) it comes back to the host.
OwnStep HostOnRelease(const OwnState& s, std::uint8_t release_reason, std::uint32_t epoch) noexcept;
// Host: no frame from the guest owner for `silent_ms` (its world reloaded, it left): back to the host after 10 s.
OwnStep HostOnSilence(const OwnState& s, double silent_ms) noexcept;
// Guest: a GRANT / DENY from the host (older epochs are ignored; equal epochs repeat the state).
OwnState GuestOnAnswer(const OwnState& s, const Own& m, bool* changed) noexcept;
// Guest: a host frame with a newer epoch means the host owns it (it took it back).
OwnState GuestOnHostFrame(const OwnState& s, std::uint32_t frame_epoch, bool* changed) noexcept;
// Who may send frames for it, and whose frames the other side follows: the receiver accepts a frame when the sender is
// the owner it knows. The host accepts the guest's only with the current epoch; the guest accepts the host's when the host
// owns it, or a newer epoch (GuestOnHostFrame).
bool HostAcceptsGuestFrame(const OwnState& s, std::uint32_t frame_epoch) noexcept;

// ---------------------------------------------------------------- the host's checks on the guest's word (review fixes)
// The host grants a CLAIM only when the partner body (the guest's Sam as the host sees it) is at the vehicle: within
// kClaimNearM of its footprint for a get-on, kPushNearM for a push. Without the partner's place: refused.
inline constexpr double kClaimNearM = 3.0, kPushNearM = 6.0;
struct Footprint;
bool ClaimNear(const Footprint& vehicle, bool have_partner, const double (&partner)[3], std::uint8_t claim_reason) noexcept;
// A guest-owned vehicle's frame on the host moves the host's unit only within reach: its place within kFrameReachM of the
// host unit's place or of the partner body (one bad frame never teleports a host vehicle across the map).
inline constexpr double kFrameReachM = 40.0;
bool FrameInReach(const double (&frame)[3], bool have_unit, const double (&unit)[3], bool have_partner, const double (&partner)[3]) noexcept;
// While the guest owns a parked vehicle (no DRIVE frame for kAwayIdleMs) and the partner body stays farther than kAwayM from
// it for kAwayMs, the host takes it back.
inline constexpr double kAwayM = 30.0, kAwayMs = 5000.0, kAwayIdleMs = 2000.0;
// The parked correction's teleports that did not hold (the error back over the band right after): the gap before the next
// one, 2 s for the first two, then doubling up to 60 s.
double ParkedTeleportGapMs(unsigned failed) noexcept;
// A request transform's rotation rows: unit length (within 10 %) and orthogonal (|dot| < 0.1).
bool RotRowsOk(const float (&rows)[9]) noexcept;

// Under 0.2 m/s for 1 s (M2.7: the owner's vehicle at rest).
class RestDetector {
public:
    bool Update(double speed_mps, double now_ms) noexcept;  // true while at rest
    void Reset() noexcept { still_since_ = -1; }
    bool AtRest() const noexcept { return at_rest_; }

private:
    double still_since_ = -1;
    bool at_rest_ = false;
};

// ---------------------------------------------------------------- correction bands (M2.6)
enum class Band : std::uint8_t {
    none,      // < 0.1 m and < 3 deg
    impulse,   // < 2 m (and < 45 deg): the game's net correction (velocity drive), or the clamped nudge
    interp,    // 2..5 m (or >= 45 deg): the interpolated teleport branch
    teleport,  // > 5 m: RequestTeleport, only where the vehicle fits
};
const char* BandName(Band b) noexcept;
struct BandLimits {
    double ignore_m = 0.1, ignore_deg = 3.0, impulse_m = 2.0, impulse_deg = 45.0, interp_m = 5.0;
};
Band ChooseBand(double err_m, double err_deg, const BandLimits& l = BandLimits{}) noexcept;
// The clamped velocity nudge: the velocity that closes `err` (target - current, m) in `tau_s`, at most `max_mps`, added to
// the measured velocity, the sum at most |measured| + max_mps.
void NudgeVelocity(const double (&err)[3], const float (&measured)[3], double tau_s, double max_mps, float (&out)[3]) noexcept;

// ---------------------------------------------------------------- the driven follower (v0.21, docs section 23.25)
// The target between two DRIVE frames: cubic Hermite on the position with the frames' velocities as tangents (`h_s` the
// key gap in seconds, `f` 0..1). Velocities that are not finite or over 100 m/s give the straight line instead.
void HermitePos(const double (&pa)[3], const float (&va)[3], const double (&pb)[3], const float (&vb)[3], double h_s, double f,
                double (&out)[3]) noexcept;
// Past the newest frame (late or lost frames): dead reckoning from it, at most kExtrapolateMaxMs (then held).
inline constexpr double kExtrapolateMaxMs = 250.0;
void Extrapolate(const double (&p)[3], const float (&vel)[3], double ahead_ms, double (&out)[3]) noexcept;

// The follower's velocity correction, every tick (applied through the chassis body's SetLinearAndAngularVelocity, on top
// of the game's own physics and the replayed inputs): only the velocity in the plane of the vehicle's up axis and the yaw
// rate are steered; vertical velocity, pitch and roll stay the game's (suspension, slopes). Along its forward axis:
//   v_des = v_target + clamp(Kp * e_along + I, max_corr)      I += Ki * e_along * dt (|I| <= max_bias)
//   dv    = clamp(beta * (v_des - v_now), max_dv)            beta = 1 - exp(-dt / tau)
// sideways only kp_lat and at most max_dv_lat a tick (a sideways push tips a truck over), the side error mostly through
// the yaw (below);
//   w_des = w_target_z + clamp(Kyaw * e_yaw, kMaxYawCorr)    dw = clamp(beta * (w_des - w_now_z), kMaxDwRad)
// Gains x kFarGain from kFarM (a larger error closes faster). The integral is cleared while both stand still.
// The tyres take a sideways velocity back within a few frames (hidden run 20261120-155059: a 1 m step along closed in
// 0.8 s, sideways only 50 % in 1.5 s), so a sideways error is also steered away like a driver would: the yaw target turns
// toward a point `lat_ahead_s` x speed (at least lat_min_m) ahead, by at most lat_max_rad, while it moves over
// lat_min_speed_mps (reversing: the other way). v0.21: 0.5 m/s, was 1 m/s (hidden run 20261120-210244: a roadster driven
// slowly kept 0.57 m of side error through a whole stop; under 1 m/s nothing steered it away).
struct FollowGains {
    double kp = 2.0, ki = 0.6, kyaw = 2.0, tau_s = 0.12, kp_lat = 0.5;
    double max_corr_mps = 3.0, max_bias_mps = 1.0, max_dv_mps = 0.25, max_dv_lat_mps = 0.05, max_yaw_corr = 1.0, max_dw_rad = 0.3;
    double far_m = 1.5, far_gain = 1.5;
    double lat_ahead_s = 0.7, lat_min_m = 3.0, lat_max_rad = 0.35, lat_sign = 1.0, lat_min_speed_mps = 0.5;
    // the steering input trim (vehicle_drive.cpp adds it to the replayed steering axis): the yaw error the controller
    // steers, x k_steer per rad, at most max_steer (the tyres take the chassis back to where the wheels point: the yaw is
    // turned through the wheels; hidden run 20261120-162308)
    double k_steer = 2.0, max_steer = 0.3;
};
// The steering trim (positive: steer right) for the controller's yaw command: -k_steer * yaw_cmd, reversing the other way,
// clamped; 0 under lat_min_speed_mps along.
double SteerTrim(double yaw_cmd, double v_along, const FollowGains& g) noexcept;
struct FollowIn {
    double err[3] = {};        // target - ours (world, the entity origins)
    float up[3] = {0, 0, 1};   // our vehicle's up axis (unit)
    float fwd[3] = {0, 1, 0};  // ... and its forward axis (unit)
    float v_now[3] = {};       // our chassis body's velocity (world)
    float v_target[3] = {};    // the owner's (interpolated)
    double yaw_err = 0;        // target yaw - ours (rad, about world z, wrapped)
    float w_now_z = 0, w_target_z = 0;  // yaw rates (rad/s, world z)
    double dt_s = 1.0 / 30;
    bool still = false;        // both stand still (the integral is cleared, no bias)
    bool yaw = true;           // the yaw rate is steered (false: dw 0, the yaw rate left as it is)
};
struct FollowOut {
    float v_set[3] = {};       // the velocity to set (v_now + dv)
    float w_set_z = 0;         // the yaw rate to set (w_now_z + dw)
    float dv[3] = {};
    float dw = 0;
    double yaw_cmd = 0;        // the yaw error the controller steers (the heading error plus the pursuit's), rad
};
// `bias` is the integral (horizontal, world), kept by the caller between ticks.
void FollowVelocity(const FollowIn& in, const FollowGains& g, double (&bias)[3], FollowOut* out) noexcept;
// The vehicle's yaw (rad, about world z) from its forward row (rows[3..5]).
double YawOfRows(const float (&rows)[9]) noexcept;
double WrapPi(double a) noexcept;

// The follower lets go once the owner's DRIVE frames have stopped this long (the play time past the newest frame);
// v0.21: kStallEndCarriedMs while our Sam is carried on it (ride_wire's kLinkSilentCarriedMs: a stall is ridden out on
// the seat, the vehicle rolls to a stop on idle inputs).
inline constexpr double kStallEndMs = 1500.0, kStallEndCarriedMs = 4500.0;
// ... and, carried, the newest frame's inputs are kept until the play time is this far past it (then idle inputs)
inline constexpr double kStallHoldInputsMs = 1500.0;

// When the driven follower is put onto its owner's place by RequestTeleport (a visible jump; never otherwise):
//   far     over kTpFarM (kTpFarCarriedM while our Sam is carried on it);
//   angle   over kTpAngleDeg (kTpAngleCarriedDeg carried);
//   stuck   over kTpStuckM for kTpStuckMs without getting under kTpStuckFall of where it started (a rock only one world
//           has); never while carried (only `far`);
//   stopped the owner's vehicle stands still for kTpParkedMs and ours is over kTpParkedM or kTpParkedDeg off (never
//           dragged there: hidden run 20261120-160609 pushed a truck lying askew into the Sam at its door); or both stand
//           still for kTpStoppedMs and it is over kTpStoppedM off, nobody carried and our Sam over kTpStoppedNearM from
//           it (a stop the replay ended a little apart: the controller had its 1.5 s first); or, with our Sam carried on
//           it (v0.21), both stand still for kTpStoppedMs, the owner's for kTpParkedMs, and it is over kTpStoppedM or
//           kTpParkedDeg off: a re-seat at a standstill (ride_together re-seats the passenger in the same tick; hidden run
//           20261120-210244 stood 0.57 m off for a whole stop, and the next drive began with that correction).
// At most one every kTpGapMs.
enum class DriveTp : std::uint8_t { none = 0, too_far, angle, stuck, stopped };
const char* DriveTpName(DriveTp t) noexcept;
inline constexpr double kTpFarM = 4.0, kTpFarCarriedM = 8.0, kTpAngleDeg = 30.0, kTpAngleCarriedDeg = 60.0;
inline constexpr double kTpStuckM = 1.5, kTpStuckMs = 2000.0, kTpStuckFall = 0.8;
inline constexpr double kTpStoppedM = 0.3, kTpStoppedMs = 1500.0, kTpStoppedNearM = 3.0, kTpGapMs = 2000.0;
inline constexpr double kTpParkedM = 1.0, kTpParkedDeg = 10.0, kTpParkedMs = 500.0;
struct DriveTpIn {
    double err_m = 0, ang_deg = 0;
    bool carried = false;          // our Sam rides it as the passenger
    double stuck_ms = 0;           // over kTpStuckM since (0: not)
    double stuck_err0 = 0;         // the error when that began
    double still_ms = 0;           // both stand still since (0: not)
    double owner_still_ms = 0;     // the owner's vehicle stands still since (0: not)
    double sam_dist_m = 1e9;       // our Sam (on foot) to the vehicle
    double since_last_tp_ms = 1e9;
};
DriveTp DriveTeleport(const DriveTpIn& in) noexcept;

// A vehicle's footprint for the teleport clearance: an oriented box (rows right, forward, up) around its centre.
struct Footprint {
    double c[3] = {};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float half[3] = {};  // metres along right, forward, up
};
// Half extents by kind (roadster, truck, trike), with a margin.
void KindHalfExtents(std::uint8_t kind, float (&half)[3]) noexcept;
bool PointInFootprint(const Footprint& f, const double (&p)[3], double margin_m) noexcept;
// Two footprints overlap (2D separating axes on the ground plane, plus their height ranges).
bool FootprintsOverlap(const Footprint& a, const Footprint& b) noexcept;
// RequestTeleport is allowed only where the target footprint holds neither the local Sam (with 0.6 m) nor another vehicle.
bool TeleportClear(const Footprint& target, bool have_sam, const double (&sam)[3], const Footprint* others, std::size_t n_others) noexcept;

// Rotation rows <-> quaternion (x, y, z, w), the conventions of pose_wire's QuatFromMatrix / MatrixFromQuat.
void QuatFromRows(const float (&rows)[9], float (&q)[4]) noexcept;
void RowsFromQuat(const float (&q)[4], float (&rows)[9]) noexcept;
// The angle between two rotations (deg).
double AngleBetweenRows(const float (&a)[9], const float (&b)[9]) noexcept;

}  // namespace st::vwire
