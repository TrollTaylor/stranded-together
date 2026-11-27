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
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

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

// ---------------------------------------------------------------- the action
// truck_v3 <op>: 0 off (+ status), 1 on [period s], 8 the seat dump, 9 the identity dump. PeriodOf clamps the watch period.
enum class Op : std::uint8_t { off, on, seats, ident, bad, v3 };  // v3: round 28 T1's status (truck_v3 3)
inline Op OpOf(double v) {
    if (!std::isfinite(v) || std::fabs(v) > 1000) return Op::bad;
    const int i = static_cast<int>(v);
    if (static_cast<double>(i) != v) return Op::bad;
    return i == 0 ? Op::off : i == 1 ? Op::on : i == 3 ? Op::v3 : i == 8 ? Op::seats : i == 9 ? Op::ident : Op::bad;
}
inline double PeriodOf(double v) { return !std::isfinite(v) || v <= 0 ? 1.0 : v < 0.2 ? 0.2 : v > 10 ? 10.0 : v; }

}  // namespace st::truckv3
