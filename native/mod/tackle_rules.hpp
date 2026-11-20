// Vehicle hits (design milestone M4; docs/real-partner-body.md section 25): the pure part. The rules of partner_tackle.cpp
// (when the partner's vehicle may hit our Sam, which game path a vehicle kind uses, the damage cap, the rate limit, which
// hit points count as our Sam's, the hit sphere), the optional hit event to the driver ("STH1", one body inside an SVM1
// message of external type 49 on the link's reliable ordered channel 3), and the partner capsule's vehicle rules (M4.4).
// No game memory, no link, no Windows: unit-tested in tests/tackle_rules_tests.cpp.
//
// The victim's game decides the hit on its own Sam (M4.2). On the victim's screen the partner-driven vehicle is our
// follower (the game's NPC controller, driver 0x1FFFFF), which the game's VehicleTackleEmit never lets hit anything. The mod
// runs the game's own vehicle attack for it:
//   direct    the game's attack sphere query 0x1429934E0 with the vehicle's attack context (trucks: VehicleTackle 0x157, the
//             MULE truck attack: 100 damage, reaction Blow, slot +0x9E0; trike and roadster: VehicleTacklePlayer 0x158, slot
//             +0xA00), the vehicle's own sphere, the game's filter (the vehicle's own bodies left out) and a callback that
//             keeps only hits within 1.5 m of our Sam (decision D4-3: the partner's vehicle hits only you), amount 100 at
//             any speed (D4-2: no instant kill). Every kind, whenever its checks passed.
//   bracket   the fallback when the direct path's checks failed: the proven state-4 bracket (run
//             20261118-224729-zz-proof-hit3): the vehicle's ride state +0x4CC = 4 for one call of the game's
//             VehicleTackleEmit 0x142F8ABF0 and back; it selects VehicleTacklePlayer 0x158 (slot +0xA00); the speed field is
//             held at 29.5 km/h for the call above 30 (the game passes 10000 there; D4-2). Whatever the game's sphere catches
//             is hit, so it is called only while our Sam's centre is inside that sphere.
#pragma once
#include <cstddef>
#include <cstdint>

namespace st::trules {

// ---------------------------------------------------------------- the game's numbers (VehicleTackleEmit, [V])
inline constexpr double kRangeM = 50.0;              // the game's MULE gate: the local player within 50 m (dist^2 <= 2500)
inline constexpr double kMinSpeedKmh = 15.0;         // speed gate: [[V+0x330]+0xF0] > 15 (km/h, proven)
inline constexpr double kLethalSpeedKmh = 30.0;      // above: the game's amount is 10000 instead of 100
inline constexpr float kHitAmount = 100.0f;          // the knock-down hit: the game's own amount at or below 30 km/h
inline constexpr float kBracketSpeedKmh = 29.5f;     // the speed field during a capped bracket call
inline constexpr std::uint16_t kAttackMule = 0x157;  // VehicleTackle (slot +0x9E0, trucks)
inline constexpr std::uint16_t kAttackPlayer = 0x158;  // VehicleTacklePlayer (slot +0xA00, every vehicle)

// ---------------------------------------------------------------- the mod's rules
inline constexpr double kHoldOffMs = 1500.0;         // after a hit from a vehicle: no call for it this long
inline constexpr double kPassIdleMs = 1000.0;        // a pass without a call this long is over (no hit): nothing later counts for it
inline constexpr double kRestrictM = 1.5;            // a hit point is our Sam's within this of his centre
inline constexpr double kSamCentreUpM = 1.0;         // his centre: the root (feet) plus this up
inline constexpr double kNearSphereM = 2.0;          // calls only while our Sam's centre is within the sphere radius + this
inline constexpr double kCrashDelayMs = 150.0;       // D3-3: the hit after a crash exit waits for the placement this long
inline constexpr double kCrashWindowMs = 1000.0;     // ... and is tried until a hit is seen or this long has passed
inline constexpr double kCrashSphereM = 1.0;         // ... its sphere's radius
inline constexpr double kCrashSphereBackM = 0.6;     // ... its centre this far from our Sam's centre toward the vehicle

enum class Path : std::uint8_t { none = 0, direct = 1, bracket = 2 };
const char* PathName(Path p) noexcept;

// Which path a vehicle kind (1 roadster, 2 truck, 3 trike) uses (decision D4-1): `forced` 0 auto, 1 direct, 2 bracket (debug
// action / tests). Auto: the direct path whenever its checks passed (with the MULE attack where the vehicle has its slot
// +0x9E0, trucks; else the player attack +0xA00): it alone keeps the hit to our Sam (D4-3); else the bracket where its
// checks passed; else none. `has_mule_slot` only picks the attack (AttackOf).
Path PathFor(std::uint8_t kind, bool has_mule_slot, bool direct_ok, bool bracket_ok, int forced) noexcept;
// The attack a path uses on that vehicle: direct with the MULE slot 0x157, else 0x158.
std::uint16_t AttackOf(Path p, bool has_mule_slot) noexcept;

// ---------------------------------------------------------------- the gate (PartnerTackle, M4.2)
enum class Block : std::uint8_t {
    ok = 0,
    off,               // the feature is off (checks, config)
    friendly_fire,     // session.ini partner_vehicle_damage=0 (D4-4)
    no_vehicle,        // the partner drives nothing we follow
    not_driven,        // the partner's frames do not say it holds the driver seat, or the vehicle has a driver of its own
    no_local,          // no local Sam with a transform
    asleep,            // our Sam is asleep (a menu)
    not_on_foot,       // our Sam has a parent (he drives / rides something of the game's)
    carried,           // our Sam rides as a passenger (riding together)
    game_state,        // a load, a menu, a cutscene, the post-load setup, a sequence (LocalCollisionBlock)
    too_far,           // over 50 m from the vehicle
    slow,              // the vehicle at 15 km/h or less
    hold_off,          // a hit from this vehicle less than 1.5 s ago
    not_near,          // our Sam is not near the vehicle's attack sphere (the bracket: not inside it)
    no_path,           // no path for this kind
    count
};
const char* BlockName(Block b) noexcept;

struct GateIn {
    bool feature_on = false;
    bool friendly_fire = true;
    bool have_vehicle = false;
    bool partner_drives = false;      // the partner's newest frame: seated driver, and the vehicle's driver id is 0x1FFFFF
    bool local_ok = false;
    bool local_awake = false;
    bool local_on_foot = false;
    bool carried = false;
    bool game_blocked = false;        // LocalCollisionBlock said why not
    double dist_m = 1e9;              // our Sam's root to the vehicle's origin
    double speed_kmh = 0;             // the vehicle's speed field
    double now_ms = 0;
    double hold_off_until_ms = 0;
    double sam_to_sphere_m = 1e9;     // our Sam's centre to the attack sphere's centre
    double sphere_radius_m = 0;
    Path path = Path::none;
};
Block Gate(const GateIn& in) noexcept;

// ---------------------------------------------------------------- the hit
// The game's sphere centre in the vehicle's frame: the local offset [V+0xA70] with its forward (y) component negated while
// the signed speed [V+0x580] is below 0 (reversing), then the vehicle's transform (rows right, forward, up).
void SphereCentre(const double (&pos)[3], const float (&rows)[9], const float (&local)[3], float signed_speed, double (&out)[3]) noexcept;
// Our Sam's centre (the root plus kSamCentreUpM up).
void SamCentre(const double (&root)[3], double (&out)[3]) noexcept;
// A hit point (DamageInfo+0x80, three doubles) counts as our Sam's: finite and within kRestrictM of his centre.
bool HitIsSams(const double (&hit)[3], const double (&sam_centre)[3]) noexcept;
// The amount a hit may carry (D4-2): above kHitAmount it is capped there; negative / NaN stays as it is (the game reads the
// attack's own damage then, which is 100 / 400: never lethal).
float CapAmount(float amount) noexcept;
// The bracket path: the speed field to hold for the call, or a negative value when it may stay (at or below 30 km/h).
float BracketSpeed(float speed_kmh) noexcept;
// D3-3: the sphere for the hit after a crash exit: centred kCrashSphereBackM from our Sam's centre toward the vehicle's
// origin (horizontally), radius kCrashSphereM.
void CrashSphere(const double (&sam_root)[3], const double (&vehicle_pos)[3], double (&centre)[3]) noexcept;

// The rate limit, per vehicle: call while the gate passes until a hit from it is seen, then hold off kHoldOffMs. A pass
// (calls until a hit) is counted for the log ("<= 3 damage messages per pass" is the hidden runs' check).
class HitLimiter {
public:
    void Reset() noexcept { *this = HitLimiter{}; }
    // A call is about to be made now.
    void Called(double now_ms) noexcept;
    // Sam's damage handler (or a health drop) saw a hit from this vehicle.
    void HitSeen(double now_ms) noexcept;
    // Once a tick: a pass whose last call is over kPassIdleMs old ends without a hit (true when it just ended).
    bool Expire(double now_ms) noexcept;
    double HoldOffUntil() const noexcept { return hold_until_; }
    bool InPass() const noexcept { return in_pass_; }
    unsigned CallsThisPass() const noexcept { return calls_; }
    unsigned Passes() const noexcept { return passes_; }
    unsigned Hits() const noexcept { return hits_; }
    double PassStartMs() const noexcept { return pass_start_; }

private:
    bool in_pass_ = false;
    double pass_start_ = 0, hold_until_ = 0, last_call_ = 0;
    unsigned calls_ = 0, passes_ = 0, hits_ = 0;
};

// ---------------------------------------------------------------- the hit event (STH1, victim -> driver, optional)
inline constexpr std::uint8_t kSvmType = 49;
inline constexpr std::uint8_t kMagic[4] = {'S', 'T', 'H', '1'};
inline constexpr std::size_t kSvmHeaderSize = 8;
inline constexpr std::size_t kBodySize = 32;
inline constexpr std::size_t kMessageSize = kSvmHeaderSize + kBodySize;
struct HitMsg {
    std::uint8_t from = 0;            // 0 host, 1 guest (the victim)
    std::uint8_t kind = 0;            // the vehicle's kind (1..3)
    std::uint8_t path = 0;            // Path
    std::uint8_t crash = 0;           // 1: the passenger thrown off in a crash (D3-3)
    std::uint32_t vehicle_id = 0;     // the host's id (VKey)
    std::uint16_t attack = 0;         // 0x157 / 0x158
    std::uint16_t world = 0;          // the victim's world counter (low 16 bits; vshare::WorldReadyNow)
    std::uint32_t sender_ms = 0;
    float speed_kmh = 0;
    float health_before = 0, health_after = 0;
};
// Body (32 bytes): "STH1" | u8 from | u8 kind | u8 path | u8 crash | u32 vehicle_id | u16 attack | u16 world | u32 sender_ms
// | f32 speed_kmh | f32 health_before | f32 health_after.
std::size_t EncodeHit(const HitMsg& m, std::uint8_t* out, std::size_t cap) noexcept;
bool DecodeHit(const std::uint8_t* in, std::size_t size, HitMsg& out) noexcept;

// ---------------------------------------------------------------- the partner capsule and vehicles (M4.4)
inline constexpr double kCapsuleVehicleNearM = 5.0;  // off while a vehicle moving over 1 m/s is within this of the body
inline constexpr double kCapsuleVehicleMps = 1.0;      // a vehicle starts to count as moving over this (m/s)
inline constexpr double kCapsuleVehicleStopMps = 0.6;  // ... and stops at or below this (the hysteresis)
inline constexpr double kCapsuleRadiusM = 0.4;       // the partner capsule's radius (Sam's mover resource)
struct VehicleBox {
    double c[3] = {};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float half[3] = {};               // metres along right, forward, up
    double speed_mps = 0;
    bool moving = false;              // Moving's answer (the hysteresis); speed_mps over 1 m/s counts too
};
// The moving flag of a vehicle with the hysteresis: on over kCapsuleVehicleMps, off at or below kCapsuleVehicleStopMps.
bool Moving(bool was_moving, double speed_mps) noexcept;
// A vehicle's speed from two of its sampled places dt_ms apart (the drive module's poll times), averaged half and half
// with the previous estimate; a bad sample (no time, not finite, 200 m/s or more) keeps the previous one.
double SpeedSample(double prev_speed, bool have_prev, const double (&from)[3], const double (&to)[3], double dt_ms) noexcept;
// The distance from a point to a box (0 inside).
double DistanceToBox(const VehicleBox& b, const double (&p)[3]) noexcept;
// Why the partner capsule must stay out of the world because of a vehicle, or nullptr: a vehicle moving (over 1 m/s, or
// its `moving` flag) within
// 5 m of the body; with `enabling` (the capsule would go back in), any vehicle whose box overlaps the capsule (the body's
// point and 1 m / 2 m above it, within the capsule's radius).
const char* CapsuleVehicleBlock(const double (&body)[3], const VehicleBox* boxes, std::size_t n, bool enabling) noexcept;

}  // namespace st::trules
