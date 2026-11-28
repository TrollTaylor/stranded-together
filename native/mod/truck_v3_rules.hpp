// Round 28 P2 (trucks M0, docs/enemy-sync.md 14.20): the pure part of the truck controller probe (truck_v3.cpp). Header-only,
// unit-tested (tests/truck_v3_rules_tests.cpp). Nothing here reads game memory.
//
//   IdentityOf   what truck_v3 9 read for one MULE-driven vehicle -> the verdict: is the vehicle's input object [vehicle+0x528]
//                the driver MULE's own controller input (actor +0x3D0 + 0x30 = actor +0x400), and do the MULE's ride fields
//                (+0x3C0 seat, +0xE20 vehicle id, +0xE28 vehicle handle, +0xE44 bit 0 riding) and its controller (+0x18 state,
//                +0x20 driver id, +0x00 the vehicle handle, +0x100 the AI drive component) agree with it (plan Q2).
//   LocalOf      a seated MULE's world transform in its truck's frame (rows convention of vehicle_sync.cpp: world =
//   SeatWorld    truck.pos + sum_j local[j] * truck.rot.m[j]) and back (plan Q7, `truck_v3 8`).
//   GetOffSite   a get-off detour's return address -> the call site and its known caller name (0x142AFB010's six callers).
//   RideChanged  the watch's change test (`truck_v3 1`): a board / leave / seat / controller-state change is logged.
//   STO1         round 29 T2 (vehicle_mule_seat): the host's truck occupants (EncodeOcc / DecodeOcc, SVM1 type 52), their entries
//                (OccEntryOf: the parent chain's local transform or the seat table), the guest's show / hide decision (SeatDecide),
//                the seat placement (SeatPlaceOf), its meter's sample rule and the truck-shove rule.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "vehicle_truck_rules.hpp"

namespace st::truckv3 {

inline constexpr std::uint32_t kNoActor = trucks::kNoActor;  // 0x1FFFFF
inline constexpr std::uintptr_t kImage = 0x140000000ull;

// ---------------------------------------------------------------- Q2: the controller identity
// What the probe read for one vehicle whose driver seat (+0x63C, +0x8B0) names a MULE. Absolute addresses (this process).
struct CtlRead {
    std::uint32_t truck_id = 0;          // the unit's game-actor id
    std::uintptr_t truck_entity = 0;     // the vehicle entity
    std::int32_t has_driver = 0;         // vehicle +0x63C
    std::uint32_t driver_id = kNoActor;  // vehicle +0x8B0
    std::uintptr_t input = 0;            // [vehicle +0x528]
    std::uintptr_t input_vt = 0;         // [[vehicle +0x528]]
    std::uintptr_t want_vt = 0;          // the NPC controller's input vtable (0x143E229B8, relocated)
    bool actor_found = false;            // the registry has an in-use actor with driver_id
    bool actor_is_mule = false;          // ... and it is a DSMuleGameActor (the ride layout below is the MULE's)
    bool ride_ok = false;                // the ride-field byte checks passed (else the ride / ctl fields are not read)
    std::uintptr_t actor = 0;
    std::uintptr_t ctl30 = 0;            // actor + 0x3D0 + 0x30
    std::int32_t ctl_state = -1;         // ctl +0x18 (= ride +0x28): 0 none, 1, 2 attached, 3 engine on
    std::uint32_t ctl_driver = kNoActor; // ctl +0x20 (Attach's driver id)
    std::uintptr_t ctl_vehicle = 0;      // [ctl +0x00] - 0x20 (Attach keeps the vehicle's handle there)
    std::uintptr_t drive_comp = 0;       // ctl +0x100 (Attach's AI drive component lookup, key 0x144B35D30)
    std::int32_t seat = -1;              // actor +0x3C0 (ride +0)
    std::uint32_t ride_vehicle_id = kNoActor;  // actor +0xE20 (ride +0xA60)
    std::uintptr_t ride_vehicle = 0;     // [actor +0xE28] - 0x20 (ride +0xA68)
    std::uint8_t ride_bits = 0;          // actor +0xE44 (ride +0xA84; bit 0 riding)
};

enum class Identity : std::uint8_t {
    same,          // [vehicle+0x528] == actor+0x400: the MULE's 48 input bytes are what the truck reads
    differs,       // another input object (a follower, a player's, a script's)
    no_input,      // [vehicle+0x528] is null
    no_actor,      // no in-use actor with the driver id
    not_mule,      // the driver is not a DSMuleGameActor (another non-player)
    unchecked,     // the ride / controller byte checks failed: nothing compared
};
inline const char* IdentityName(Identity v) {
    switch (v) {
        case Identity::same: return "SAME";
        case Identity::differs: return "DIFFERS";
        case Identity::no_input: return "no input object";
        case Identity::no_actor: return "no actor";
        case Identity::not_mule: return "not a MULE";
        case Identity::unchecked: return "unchecked";
    }
    return "?";
}

// The secondary facts that should agree when the identity holds (a bit each; 0 = all agree).
enum Mismatch : unsigned {
    kMisInputVt = 1u,      // the input object's vtable is not the NPC controller's
    kMisCtlDriver = 2u,    // ctl +0x20 is not the driver id
    kMisCtlVehicle = 4u,   // ctl +0x00 is not this vehicle
    kMisCtlState = 8u,     // ctl +0x18 is not 2 / 3 (attached / engine on)
    kMisRideId = 16u,      // actor +0xE20 is not this vehicle's id
    kMisRideHandle = 32u,  // actor +0xE28 is not this vehicle
    kMisNotRiding = 64u,   // actor +0xE44 bit 0 is clear
    kMisSeat = 128u,       // actor +0x3C0 is not 0 (the driver seat)
    kMisNoDriveComp = 256u,  // ctl +0x100 is null (no AI drive component: plan Q2's second half)
    kMisHasDriver = 512u,  // vehicle +0x63C is 0 while +0x8B0 names the MULE
};
struct Verdict {
    Identity id = Identity::unchecked;
    unsigned mis = 0;
    bool Ok() const noexcept { return id == Identity::same && mis == 0; }
};
inline Verdict IdentityOf(const CtlRead& r) {
    Verdict v;
    if (!r.actor_found) return v.id = Identity::no_actor, v;
    if (!r.actor_is_mule) return v.id = Identity::not_mule, v;
    if (!r.ride_ok) return v.id = Identity::unchecked, v;
    if (!r.input) v.id = Identity::no_input;
    else v.id = r.input == r.ctl30 ? Identity::same : Identity::differs;
    if (r.input && r.input_vt != r.want_vt) v.mis |= kMisInputVt;
    if (r.ctl_driver != r.driver_id) v.mis |= kMisCtlDriver;
    if (r.ctl_vehicle != r.truck_entity) v.mis |= kMisCtlVehicle;
    if (r.ctl_state != 2 && r.ctl_state != 3) v.mis |= kMisCtlState;
    if (r.ride_vehicle_id != r.truck_id) v.mis |= kMisRideId;
    if (r.ride_vehicle != r.truck_entity) v.mis |= kMisRideHandle;
    if (!(r.ride_bits & 1u)) v.mis |= kMisNotRiding;
    if (r.seat != 0) v.mis |= kMisSeat;
    if (!r.drive_comp) v.mis |= kMisNoDriveComp;
    if (r.has_driver == 0) v.mis |= kMisHasDriver;
    return v;
}
// "vt ctl-driver ..." for the log (empty when none).
inline void MismatchText(unsigned mis, char* out, std::size_t cap) {
    if (!out || !cap) return;
    static const char* const kNames[] = {"input-vtable", "ctl-driver", "ctl-vehicle", "ctl-state", "ride-id", "ride-handle", "not-riding", "seat",
                                         "no-drive-comp", "no-+0x63C"};
    std::size_t n = 0;
    out[0] = 0;
    for (unsigned b = 0; b < sizeof(kNames) / sizeof(kNames[0]); ++b) {
        if (!(mis & (1u << b))) continue;
        for (const char* c = n ? " " : ""; *c && n + 1 < cap; ++c) out[n++] = *c;
        for (const char* c = kNames[b]; *c && n + 1 < cap; ++c) out[n++] = *c;
    }
    out[n < cap ? n : cap - 1] = 0;
}

// The run's identity tally (truck_v3 9 and the watch): the gate wants same == checked and differs == 0.
struct Tally {
    unsigned checked = 0, same = 0, differs = 0, other = 0, agree = 0, no_drive_comp = 0;
    void Add(const Verdict& v) {
        ++checked;
        if (v.id == Identity::same) ++same;
        else if (v.id == Identity::differs) ++differs;
        else ++other;
        if (v.Ok()) ++agree;
        if (v.mis & kMisNoDriveComp) ++no_drive_comp;
    }
};

// ---------------------------------------------------------------- Q7: seat offsets
struct Xf {
    double pos[3] = {};
    float rot[3][3] = {};  // rows, as game::RotMatrix (the engine's 0x3C-byte WorldTransform)
};
// `w` in the frame of `v`: local[j] = rot_v[j] . (w.pos - v.pos); local rows r[i][k] = rot_w[i] . rot_v[k].
inline void LocalOf(const Xf& v, const Xf& w, double (&lpos)[3], double (&lrot)[3][3]) {
    const double d[3] = {w.pos[0] - v.pos[0], w.pos[1] - v.pos[1], w.pos[2] - v.pos[2]};
    for (int j = 0; j < 3; ++j) lpos[j] = d[0] * v.rot[j][0] + d[1] * v.rot[j][1] + d[2] * v.rot[j][2];
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
            lrot[i][k] = static_cast<double>(w.rot[i][0]) * v.rot[k][0] + static_cast<double>(w.rot[i][1]) * v.rot[k][1] +
                         static_cast<double>(w.rot[i][2]) * v.rot[k][2];
}
// Back: the world transform of a seat `lpos` / `lrot` on the vehicle `v`.
inline Xf SeatWorld(const Xf& v, const double (&lpos)[3], const double (&lrot)[3][3]) {
    Xf w;
    for (int i = 0; i < 3; ++i) w.pos[i] = v.pos[i] + lpos[0] * v.rot[0][i] + lpos[1] * v.rot[1][i] + lpos[2] * v.rot[2][i];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            w.rot[i][j] = static_cast<float>(lrot[i][0] * v.rot[0][j] + lrot[i][1] * v.rot[1][j] + lrot[i][2] * v.rot[2][j]);
    return w;
}
// The local rotation's heading about the vehicle's row 2 (up): the angle of the seated forward row (1) in the vehicle's
// row-0 / row-1 plane, degrees (0 = facing the vehicle's forward, +90 = its row 0).
inline double LocalYawDeg(const double (&lrot)[3][3]) {
    const double a = std::atan2(lrot[1][0], lrot[1][1]);
    return a * 57.29577951308232;
}
inline bool XfSane(const Xf& x) {
    for (double p : x.pos)
        if (!std::isfinite(p) || std::fabs(p) > 1e7) return false;
    bool any = false;
    for (const auto& r : x.rot)
        for (float f : r) {
            if (!std::isfinite(f) || std::fabs(f) > 1.5f) return false;
            if (f != 0) any = true;
        }
    return any;
}

// ---------------------------------------------------------------- the get-off callers (0x142AFB010, read-only detour)
inline constexpr std::uintptr_t kGetOffVa = 0x142AFB010;
struct KnownCaller {
    std::uintptr_t site;  // the call instruction (E8 rel32)
    const char* name;
};
inline constexpr KnownCaller kGetOffCallers[] = {
    {0x142AF563B, "0x142AF5620"},
    {0x142AF7E37, "0x142AF7DD0"},
    {0x142B00A71, "0x142B00A02"},
    {0x142B3AA73, "0x142B3AA30"},
    {0x142B3AB63, "abort / get-out 0x142B3AB20"},
    {0x142B4506E, "0x142B4503D"},
};
// A return address (preferred-base VA) -> the call site (ra - 5) when it is a known caller, its name; else 0 / "unknown".
inline std::uintptr_t GetOffSite(std::uintptr_t ra_va, const char** name) {
    for (const KnownCaller& k : kGetOffCallers)
        if (ra_va == k.site + 5) {
            if (name) *name = k.name;
            return k.site;
        }
    if (name) *name = "unknown";
    return 0;
}
// An absolute address of this process -> its ds.exe preferred-base VA (0 when outside [base, base + size)).
inline std::uintptr_t ToVa(std::uintptr_t abs, std::uintptr_t base, std::uintptr_t size) {
    return base && abs >= base && abs - base < size ? kImage + (abs - base) : 0;
}

// ---------------------------------------------------------------- the watch (truck_v3 1)
struct Ride {
    std::uint32_t mule = 0, truck = kNoActor;
    std::int32_t seat = -1, ctl_state = -1;
    bool riding = false;
};
// What the watch logs about one MULE since its previous sample.
enum class RideEvent : std::uint8_t { none, board, leave, seat, state };
inline RideEvent RideChanged(const Ride& was, bool had, const Ride& now, bool has) {
    const bool w = had && was.riding, n = has && now.riding;
    if (!w && n) return RideEvent::board;
    if (w && !n) return RideEvent::leave;
    if (!n) return RideEvent::none;
    if (was.truck != now.truck) return RideEvent::board;  // another truck: a new boarding
    if (was.seat != now.seat) return RideEvent::seat;
    if (was.ctl_state != now.ctl_state && now.seat == 0) return RideEvent::state;
    return RideEvent::none;
}
inline const char* RideEventName(RideEvent e) {
    switch (e) {
        case RideEvent::none: return "none";
        case RideEvent::board: return "board";
        case RideEvent::leave: return "leave";
        case RideEvent::seat: return "seat";
        case RideEvent::state: return "ctl";
    }
    return "?";
}

// The vehicle side of the watch (run 20261126-235943: the MULE side alone left "0 riding" open to doubt): a vehicle's driver
// seat (+0x8B0) since its previous sample. kNoActor and above = no driver. The first sample of a vehicle with a driver is a set.
enum class DrvEvent : std::uint8_t { none, set, cleared, changed };
inline DrvEvent DriverChanged(std::uint32_t was, bool had, std::uint32_t now) {
    const bool w = had && was < kNoActor, n = now < kNoActor;
    if (!w && n) return DrvEvent::set;
    if (w && !n) return DrvEvent::cleared;
    if (w && n && was != now) return DrvEvent::changed;
    return DrvEvent::none;
}
inline const char* DrvEventName(DrvEvent e) {
    switch (e) {
        case DrvEvent::none: return "none";
        case DrvEvent::set: return "set";
        case DrvEvent::cleared: return "cleared";
        case DrvEvent::changed: return "changed";
    }
    return "?";
}
// A MULE counts as seated when its ride bit (+0xE44 bit 0) is set or its entity's parent chain (+0x70) reaches a vehicle
// (enemy_sync's first test, the one that saw 0x0202E seated in round 23's runs). The truck it names: the ride's vehicle id
// when the bit is set, else the chain's vehicle (kNoActor when the chain's vehicle is not in the pools).
inline bool SeatedOf(bool ride_bit, bool chain) { return ride_bit || chain; }
inline std::uint32_t SeatedTruck(bool ride_bit, std::uint32_t ride_vehicle_id, bool chain, std::uint32_t chain_vehicle_id) {
    if (ride_bit && ride_vehicle_id < kNoActor) return ride_vehicle_id;
    if (chain) return chain_vehicle_id;
    return ride_vehicle_id;
}

// ---------------------------------------------------------------- round 29 T2: the seated mirrors (vehicle_mule_seat)
// docs/enemy-sync.md 14.22. The host sends, for every truck it sends as V3 DRIVE frames, the MULEs riding in it: STO1 (SVM1
// external type 52, channel 3, host -> guest, only to a partner whose STV3 hello has trucks::kV3FeatSeats): the truck's host id
// and per occupant its id, seat and transform in the truck's frame (LocalOf above, rows convention). The guest shows a seated
// mirror (enemy_rules LeaseVerdict::seated) that a fresh STO1 names, in a truck she follows, at her follower's transform times
// that local transform, posed with the host's pose (SeatDecide).
//
//   STO1 header  "STO1" | u8 version 1 | u8 n (0..8) | u16 seq LE | u32 truck host id LE             (kOccHeader = 12 bytes)
//   entry        u32 MULE id | u8 seat (0..7, 0xFF unknown) | u8 src (1 the parent chain, 2 the seat table) | u16 0 |
//                f32 x y z (truck frame, m) | i16 qx qy qz qw (/32767, the local rotation)          (kOccEntry = 28 bytes)
// n = 0 is the clear: the truck has no occupants (or is no longer sent).
inline constexpr std::uint8_t kOccType = 52;  // SVM1 external type (48..51 are taken)
inline constexpr std::size_t kOccHeader = 12;
inline constexpr std::size_t kOccEntry = 28;
inline constexpr std::size_t kOccMaxPerTruck = 8;
inline constexpr std::size_t kOccMaxBody = kOccHeader + kOccEntry * kOccMaxPerTruck;  // 236
inline constexpr std::uint8_t kOccVersion = 1;
inline constexpr std::uint8_t kOccSeatUnknown = 0xFF;
inline constexpr std::uint8_t kOccSrcChain = 1, kOccSrcTable = 2;
inline constexpr double kOccMaxLocalM = 5.0;      // |x|, |y|, |z| of a seat in the truck's frame
inline constexpr double kOccLocalSaneM = 4.0;     // host: a parent-chain local transform farther than this is not used
inline constexpr double kOccFreshMs = 3000.0;     // guest: a record counts this long after its newest frame
inline constexpr double kOccKeepaliveMs = 1000.0; // host: a truck's frame again after this, changed or not
inline constexpr double kOccScanMs = 250.0;       // host: the occupant scan's period
inline constexpr double kOccMinGapMs = 100.0;     // host: a changed truck's frames at most this often

struct OccEntry {
    std::uint32_t mule = 0;
    std::uint8_t seat = kOccSeatUnknown;
    std::uint8_t src = kOccSrcChain;
    double lpos[3] = {};
    double q[4] = {0, 0, 0, 1};  // x y z w
};
struct OccFrame {
    std::uint16_t seq = 0;
    std::uint32_t truck = 0;
    std::uint8_t n = 0;
    OccEntry e[kOccMaxPerTruck];
};

namespace occ_detail {
inline void Put16(std::uint8_t* p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8); }
inline void Put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
inline std::uint16_t Get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t Get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}
inline std::int16_t QuantQ(double v) {
    const double c = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<std::int16_t>(std::lround(c * 32767.0));
}
}  // namespace occ_detail

inline bool OccTruckIdOk(std::uint32_t id) { return id >= 1 && id < kNoActor; }
inline bool OccMuleIdOk(std::uint32_t id) { return id != 0 && id < kNoActor && trucks::ActorType(id) == 1; }
// One entry's checks (the codec's both ways): the id, the seat, the source, a finite place within 5 m, a unit quaternion.
inline bool OccEntryOk(const OccEntry& e) {
    if (!OccMuleIdOk(e.mule) || (e.seat > 7 && e.seat != kOccSeatUnknown) || (e.src != kOccSrcChain && e.src != kOccSrcTable)) return false;
    for (double p : e.lpos)
        if (!std::isfinite(p) || std::fabs(p) > kOccMaxLocalM) return false;
    double n2 = 0;
    for (double c : e.q) {
        if (!std::isfinite(c)) return false;
        n2 += c * c;
    }
    return n2 >= 0.81 && n2 <= 1.21;
}
// The body (after the SVM1 header). 0 when the frame does not validate (a bad entry, a duplicate id, n > 8) or does not fit.
inline std::size_t EncodeOcc(const OccFrame& f, std::uint8_t* out, std::size_t cap) {
    using namespace occ_detail;
    if (!out || f.n > kOccMaxPerTruck || !OccTruckIdOk(f.truck)) return 0;
    const std::size_t size = kOccHeader + kOccEntry * f.n;
    if (cap < size) return 0;
    for (unsigned i = 0; i < f.n; ++i) {
        if (!OccEntryOk(f.e[i])) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (f.e[j].mule == f.e[i].mule) return 0;
    }
    std::memcpy(out, "STO1", 4);
    out[4] = kOccVersion, out[5] = f.n;
    Put16(out + 6, f.seq);
    Put32(out + 8, f.truck);
    for (unsigned i = 0; i < f.n; ++i) {
        const OccEntry& e = f.e[i];
        std::uint8_t* p = out + kOccHeader + kOccEntry * i;
        Put32(p, e.mule);
        p[4] = e.seat, p[5] = e.src, p[6] = p[7] = 0;
        for (int k = 0; k < 3; ++k) {
            const float v = static_cast<float>(e.lpos[k]);
            std::memcpy(p + 8 + 4 * k, &v, 4);
        }
        // the quaternion normalized, w >= 0 (q and -q are the same rotation)
        double n2 = 0;
        for (double c : e.q) n2 += c * c;
        const double s = (e.q[3] < 0 ? -1.0 : 1.0) / std::sqrt(n2);
        for (int k = 0; k < 4; ++k) Put16(p + 20 + 2 * k, static_cast<std::uint16_t>(QuantQ(e.q[k] * s)));
    }
    return size;
}
// Strict: the magic, version 1, n <= 8, the exact size, a truck id, every entry valid (OccEntryOk, the pad 0), no duplicate id.
// The quaternion comes back normalized.
inline bool DecodeOcc(const std::uint8_t* in, std::size_t size, OccFrame& f) {
    using namespace occ_detail;
    if (!in || size < kOccHeader || std::memcmp(in, "STO1", 4) != 0 || in[4] != kOccVersion || in[5] > kOccMaxPerTruck) return false;
    const std::uint8_t n = in[5];
    if (size != kOccHeader + kOccEntry * n) return false;
    OccFrame o;
    o.n = n, o.seq = Get16(in + 6), o.truck = Get32(in + 8);
    if (!OccTruckIdOk(o.truck)) return false;
    for (unsigned i = 0; i < n; ++i) {
        const std::uint8_t* p = in + kOccHeader + kOccEntry * i;
        OccEntry& e = o.e[i];
        e.mule = Get32(p), e.seat = p[4], e.src = p[5];
        if (p[6] != 0 || p[7] != 0) return false;
        for (int k = 0; k < 3; ++k) {
            float v = 0;
            std::memcpy(&v, p + 8 + 4 * k, 4);
            e.lpos[k] = v;
        }
        for (int k = 0; k < 4; ++k) e.q[k] = static_cast<std::int16_t>(Get16(p + 20 + 2 * k)) / 32767.0;
        if (!OccEntryOk(e)) return false;
        double n2 = 0;
        for (double c : e.q) n2 += c * c;
        const double s = 1.0 / std::sqrt(n2);
        for (double& c : e.q) c *= s;
        for (unsigned j = 0; j < i; ++j)
            if (o.e[j].mule == e.mule) return false;
    }
    f = o;
    return true;
}
// a is newer than b (u16 sequence, wrapping).
inline bool OccSeqNewer(std::uint16_t a, std::uint16_t b) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0; }

// A rotation matrix (any one convention: QuatToMat is its inverse) <-> a unit quaternion x y z w. RotOk: orthonormal within 0.05
// and not a reflection (a parent-chain read that fails it is not sent: the seat table's identity instead).
inline bool RotOk(const double (&m)[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double d = m[i][0] * m[j][0] + m[i][1] * m[j][1] + m[i][2] * m[j][2];
            if (!std::isfinite(d) || std::fabs(d - (i == j ? 1.0 : 0.0)) > 0.05) return false;
        }
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return det > 0.9;
}
inline void MatToQuat(const double (&m)[3][3], double (&q)[4]) {
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s, q[0] = (m[2][1] - m[1][2]) / s, q[1] = (m[0][2] - m[2][0]) / s, q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s, q[0] = 0.25 * s, q[1] = (m[0][1] + m[1][0]) / s, q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s, q[0] = (m[0][1] + m[1][0]) / s, q[1] = 0.25 * s, q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s, q[0] = (m[0][2] + m[2][0]) / s, q[1] = (m[1][2] + m[2][1]) / s, q[2] = 0.25 * s;
    }
    double n2 = 0;
    for (double c : q) n2 += c * c;
    const double s = (q[3] < 0 ? -1.0 : 1.0) / std::sqrt(n2 > 0 ? n2 : 1.0);
    for (double& c : q) c *= s;
}
inline void QuatToMat(const double (&q)[4], double (&m)[3][3]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - z * w), m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w), m[2][1] = 2 * (y * z + x * w), m[2][2] = 1 - 2 * (x * x + y * y);
}

// The MULE truck's seats in its frame (plan Q7, `truck_v3 8` in round 28's runs: seats 0, 1, 2, 4 measured; seat 3 inferred from
// the left / right symmetry of 1 / 2 and the row of 4). The local rotation of each: the identity (the MULE faces the truck's forward).
struct SeatPos {
    std::uint8_t seat;
    double p[3];
    bool inferred;
};
inline constexpr SeatPos kSeatTable[] = {
    {0, {-0.437, 0.810, 0.157}, false},
    {1, {1.299, 0.127, 0.778}, false},
    {2, {-1.299, 0.127, 0.778}, false},
    {3, {1.299, -0.692, 0.778}, true},
    {4, {-1.299, -0.692, 0.777}, false},
};
inline bool SeatTableLocal(std::uint8_t seat, double (&lpos)[3]) {
    for (const SeatPos& s : kSeatTable)
        if (s.seat == seat) {
            for (int i = 0; i < 3; ++i) lpos[i] = s.p[i];
            return true;
        }
    return false;
}
// The table's seat nearest `lpos` within 0.6 m, else kOccSeatUnknown (host: the seat index when the ride's is not readable).
inline std::uint8_t SeatNearest(const double (&lpos)[3]) {
    std::uint8_t best = kOccSeatUnknown;
    double bd = 0.6 * 0.6;
    for (const SeatPos& s : kSeatTable) {
        const double dx = lpos[0] - s.p[0], dy = lpos[1] - s.p[1], dz = lpos[2] - s.p[2], d = dx * dx + dy * dy + dz * dz;
        if (d < bd) bd = d, best = s.seat;
    }
    return best;
}
// Host: one occupant's entry from what it read. `chain_ok`: the MULE's and the truck's transforms were read (lpos / lrot from
// LocalOf); used when sane (within kOccLocalSaneM, RotOk), else the seat table (its position, the identity rotation). False:
// neither (an unknown seat and no usable transform: not sent).
inline bool OccEntryOf(std::uint32_t mule, std::int32_t ride_seat, bool chain_ok, const double (&lpos)[3], const double (&lrot)[3][3], OccEntry& e) {
    e = OccEntry{};
    e.mule = mule;
    bool use_chain = chain_ok && RotOk(lrot);
    for (int i = 0; use_chain && i < 3; ++i) use_chain = std::isfinite(lpos[i]) && std::fabs(lpos[i]) <= kOccLocalSaneM;
    std::uint8_t seat = ride_seat >= 0 && ride_seat <= 7 ? static_cast<std::uint8_t>(ride_seat) : kOccSeatUnknown;
    if (use_chain) {
        if (seat == kOccSeatUnknown) seat = SeatNearest(lpos);
        e.seat = seat, e.src = kOccSrcChain;
        for (int i = 0; i < 3; ++i) e.lpos[i] = lpos[i];
        MatToQuat(lrot, e.q);
        return OccEntryOk(e);
    }
    if (!SeatTableLocal(seat, e.lpos)) return false;
    e.seat = seat, e.src = kOccSrcTable;
    e.q[0] = e.q[1] = e.q[2] = 0, e.q[3] = 1;
    return OccEntryOk(e);
}
// Host: whether a truck's occupants changed enough to send now (another set, a seat, 2 cm, 2 degrees).
inline bool OccChanged(const OccFrame& a, const OccFrame& b) {
    if (a.n != b.n || a.truck != b.truck) return true;
    for (unsigned i = 0; i < a.n; ++i) {
        const OccEntry& x = a.e[i];
        const OccEntry* y = nullptr;
        for (unsigned j = 0; j < b.n && !y; ++j)
            if (b.e[j].mule == x.mule) y = &b.e[j];
        if (!y || y->seat != x.seat || y->src != x.src) return true;
        const double dx = x.lpos[0] - y->lpos[0], dy = x.lpos[1] - y->lpos[1], dz = x.lpos[2] - y->lpos[2];
        if (dx * dx + dy * dy + dz * dz > 0.02 * 0.02) return true;
        const double dot = std::fabs(x.q[0] * y->q[0] + x.q[1] * y->q[1] + x.q[2] * y->q[2] + x.q[3] * y->q[3]);
        if (dot < 0.99985) return true;  // cos(1 degree): a 2 degree turn
    }
    return false;
}

// Guest: where a seated mirror goes. `truck`: her follower's transform now; `lead`: the one tick lead along its measured velocity
// (vehicle_drive SeatTarget's: the mirror is placed before this frame's physics moves the truck). `led` is the placement, `exact`
// the seat on the truck as it is now (the next tick's meter compares the mirror's place with that tick's `exact`).
inline void SeatPlaceOf(const Xf& truck, const double (&lead)[3], const OccEntry& e, Xf& led, Xf& exact) {
    double lrot[3][3];
    QuatToMat(e.q, lrot);
    exact = SeatWorld(truck, e.lpos, lrot);
    Xf t = truck;
    for (int i = 0; i < 3; ++i) t.pos[i] += lead[i];
    led = SeatWorld(t, e.lpos, lrot);
}
inline double PosDist(const double (&a)[3], const double (&b)[3]) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
// The seat error meter's sample rule (round 29 T2 review: her follower's own corrections are not the seat's error): the mirror was
// placed at the previous GuestTick (not some earlier one), that was not its first shown tick, her follower made no correction (a
// RequestTeleport) since kSeatCorrGuardMs before that placement, and the seat moved no more than the truck's own motion explains
// (its speed times the time between the two, plus 0.3 m).
inline constexpr double kSeatJumpM = 3.0;          // the placement: a jump this far teleports the mirror instead of setting it
inline constexpr double kSeatCorrGuardMs = 150.0;  // a follower correction this soon before a placement still spoils its sample
inline constexpr double kSeatMoveSlackM = 0.3;
struct SeatSample {
    bool placed_prev_tick = false;  // placed at the GuestTick just before this one
    bool first_shown_tick = false;  // that placement was its first shown tick (the teleport)
    double placed_ms = 0;           // that placement's time
    double corr_ms = -1e18;         // her follower's newest correction of that truck
    double now_ms = 0;
    double seat_moved_m = 0;        // the seat now against the seat at that placement
    double speed_mps = 0;           // her truck's measured speed
};
inline bool SeatErrCounts(const SeatSample& s) {
    if (!s.placed_prev_tick || s.first_shown_tick || !std::isfinite(s.seat_moved_m) || !std::isfinite(s.speed_mps)) return false;
    if (s.corr_ms >= s.placed_ms - kSeatCorrGuardMs) return false;
    const double dt = s.now_ms > s.placed_ms ? (s.now_ms - s.placed_ms) / 1000.0 : 0.0;
    return s.seat_moved_m <= (s.speed_mps > 0 ? s.speed_mps : 0.0) * dt + kSeatMoveSlackM;
}

// Round 29 T2 review: a seated mirror given back (its verdict ended, a release, off) is first moved out of her truck, beside it at
// the seat's side, so its capsule never comes back inside the truck's collision; the capsule comes back once it stands clear of
// the truck's box (SeatClearOf), at once on off / reset / a fault.
inline constexpr double kSeatOutSideM = 3.0;               // |x| of the place beside the truck (its frame)
inline constexpr double kSeatClearX = 2.2, kSeatClearY = 4.5, kSeatClearZ = 4.0;  // the truck's box with a capsule's margin
inline void SeatOutLocal(const double (&lpos)[3], double (&out)[3]) {
    const double x = std::isfinite(lpos[0]) ? lpos[0] : 0.0, y = std::isfinite(lpos[1]) ? lpos[1] : 0.0;
    out[0] = x > 0.1 ? kSeatOutSideM : -kSeatOutSideM;  // the seat's side (the middle: the left, the driver's)
    out[1] = y < -2.0 ? -2.0 : y > 2.0 ? 2.0 : y;
    out[2] = 0.0;
}
inline bool SeatClearOf(const Xf& truck, const double (&p)[3]) {
    double l[3];
    const double d[3] = {p[0] - truck.pos[0], p[1] - truck.pos[1], p[2] - truck.pos[2]};
    for (int j = 0; j < 3; ++j) l[j] = d[0] * truck.rot[j][0] + d[1] * truck.rot[j][1] + d[2] * truck.rot[j][2];
    for (double c : l)
        if (!std::isfinite(c)) return false;
    return std::fabs(l[0]) > kSeatClearX || std::fabs(l[1]) > kSeatClearY || std::fabs(l[2]) > kSeatClearZ;
}

// Guest: show a seated mirror or keep it hidden (T1's hidden and asleep). First failing condition wins. Round 29 T2 review: a
// mirror is shown only with its capsule out of the world (no_capsule: kept hidden, as T1).
enum class SeatWhy : std::uint8_t { shown, off, no_record, not_followed, no_pose, no_slot, no_capsule };
inline constexpr unsigned kSeatWhyCount = 7;
struct SeatIn {
    bool on = false;         // vehicle_mule_seat, V3 live, the guest, the partner sends occupants (its hello's kV3FeatSeats)
    bool record = false;     // a fresh STO1 names this MULE
    bool followed = false;   // ... in a truck her follower holds (her copy live)
    bool host_pose = false;  // the host poses it (SEN1 kPosed) and its pose frames are here
    bool capsule = false;    // its capsule is out of the world (taken out by us this seat episode)
    bool slot = false;       // a mirror pose slot is held or was taken
};
inline SeatWhy SeatDecide(const SeatIn& s) {
    if (!s.on) return SeatWhy::off;
    if (!s.record) return SeatWhy::no_record;
    if (!s.followed) return SeatWhy::not_followed;
    if (!s.host_pose) return SeatWhy::no_pose;
    if (!s.capsule) return SeatWhy::no_capsule;
    if (!s.slot) return SeatWhy::no_slot;
    return SeatWhy::shown;
}
inline const char* SeatWhyName(SeatWhy w) {
    switch (w) {
        case SeatWhy::shown: return "shown";
        case SeatWhy::off: return "seat sync off";
        case SeatWhy::no_record: return "no occupant record";
        case SeatWhy::not_followed: return "truck not followed";
        case SeatWhy::no_pose: return "no host pose";
        case SeatWhy::no_slot: return "no pose slot";
        case SeatWhy::no_capsule: return "no capsule out of the world";
    }
    return "?";
}

// The truck-shove detector (guest, vehicle_drive's follower meter): a placement error spike while a seated mirror is shown in it
// (a mirror's collision pushing her copy would show as this).
inline constexpr double kShoveErrM = 0.75, kShoveJumpM = 0.4;
inline bool ShoveSpike(double err, double prev_err) {
    return std::isfinite(err) && std::isfinite(prev_err) && err > kShoveErrM && err - prev_err > kShoveJumpM;
}

// ---------------------------------------------------------------- the action
// truck_v3 <op>: 0 off (+ status), 1 on [period s], 8 the seat dump, 9 the identity dump. PeriodOf clamps the watch period.
// v3: round 28 T1's status (truck_v3 3); seat: round 29 T2's (truck_v3 4).
enum class Op : std::uint8_t { off, on, seats, ident, bad, v3, seat };
inline Op OpOf(double v) {
    if (!std::isfinite(v) || std::fabs(v) > 1000) return Op::bad;
    const int i = static_cast<int>(v);
    if (static_cast<double>(i) != v) return Op::bad;
    return i == 0 ? Op::off : i == 1 ? Op::on : i == 3 ? Op::v3 : i == 4 ? Op::seat : i == 8 ? Op::seats : i == 9 ? Op::ident : Op::bad;
}
inline double PeriodOf(double v) { return !std::isfinite(v) || v <= 0 ? 1.0 : v < 0.2 ? 0.2 : v > 10 ? 10.0 : v; }

}  // namespace st::truckv3
