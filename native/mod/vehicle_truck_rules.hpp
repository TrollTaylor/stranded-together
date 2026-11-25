// Round 23 D (docs/enemy-sync.md 14.10): enemy trucks and the shared real vehicles. The pure part: no game memory, no link, no
// Windows. Unit-tested in tests/vehicle_truck_rules_tests.cpp (st_vehicle_truck_rules_tests).
//
// A MULE camp's trucks are vehicles of the same pools the shared vehicles poll (kind 2). Before this the shared vehicles
// treated them like a player's truck: the owner's PARK frames and the parked / final / far teleports dragged the other game's
// copy (which its own MULE may be driving) back onto the owner's spot, and the guest's binding removed or re-mirrored it.
//
//   Classify    one vehicle from what its unit and entity say: the rider (unit +0x158; [O] FUN_142f7f810 writes it on every
//               SetDriver, also 0x1FFFFF), the +0x19D bits ([O] bit0: the last driver was a player, bit1: a non-player; sticky:
//               a 0x1FFFFF driver leaves them), the entity's driver seat (+0x63C = 1 seated, +0x8B0 the driver id; [O] SetDriver
//               0x142F7C537) and the truck's model (the resource of its entity: "truck" a player truck, "truck_npc" the MULE
//               truck, pe_ds_vehicle_celd). The mod's own follower is the game's NPC controller with driver 0x1FFFFF: seated,
//               rider none (`follower`).
//   Hold        whether vehicle_share / vehicle_drive leave a truck to each game (no PARK, no parked teleport, no removal, no
//               re-mirror) and why; a 5 s release latch (a hold ends at once only for a player driver). A truck a player drove
//               last stays shared until a non-player drives it (PlayerLast; review).
//   Facts       the "STK1" body (SVM1 external type 50, channel 3, both ways): what each game's trucks say about their
//               drivers, keyed by the host's id, so a game holds a truck the other game's MULE drives. An old build drops
//               the unknown type (vehicle_share hands types 16..63 only to registered handlers).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace st::trucks {

inline constexpr std::uint32_t kNoActor = 0x1FFFFF;          // the game's "no actor" id
inline constexpr std::uint8_t kKindTruck = 2;                // vehicle pool kind (1 roadster, 2 truck, 3 trike)
inline constexpr double kHoldReleaseMs = 5000.0;             // a hold ends this long after its last reason (not a player)
inline constexpr double kFactsExpireMs = 25000.0;            // the other game's facts are forgotten this long after they came
inline constexpr double kFactsAllMs = 10000.0;               // every truck is sent this often
inline constexpr double kFactsMinGapMs = 1000.0;             // a change is sent at most this often
inline constexpr std::uint8_t kFactsType = 50;               // SVM1 external type
inline constexpr std::size_t kFactsHeader = 8;               // "STK1" | u8 from (1 host, 2 guest) | u8 n | u16 world
inline constexpr std::size_t kFactSize = 12;                 // u32 host id | u8 model | u8 driver | u8 flags | u8 seat | u32 driver id
inline constexpr std::size_t kMaxFacts = 80;                 // 8 + 8 + 80 * 12 = 976 <= 1024 (vwire::kMaxMessage)
inline constexpr std::uint8_t kNoSeat = 0xFF;

// The actor id's type ((id >> 13) & 0xFF): 0 a player, 1 a MULE / human enemy.
inline std::uint32_t ActorType(std::uint32_t id) noexcept { return (id & 0x1FE000u) >> 13; }

enum class Model : std::uint8_t { unknown = 0, player = 1, celd = 2 };
enum class Driver : std::uint8_t { none = 0, player = 1, mule = 2, other = 3 };
inline const char* ModelName(Model m) noexcept {
    return m == Model::player ? "player" : m == Model::celd ? "MULE (celd)" : "unknown";
}
inline const char* DriverName(Driver d) noexcept {
    return d == Driver::player ? "player" : d == Driver::mule ? "MULE" : d == Driver::other ? "NPC" : "none";
}
inline Driver DriverOf(std::uint32_t id) noexcept {
    if (id >= kNoActor) return Driver::none;
    const std::uint32_t t = ActorType(id);
    return t == 0 ? Driver::player : t == 1 ? Driver::mule : Driver::other;
}
inline bool NonPlayer(Driver d) noexcept { return d == Driver::mule || d == Driver::other; }
// The model from the resource key vehicle_wire's resource table gives the entity's resource ("truck_npc" the MULE truck).
inline Model ModelOfKey(const char* key, std::uint8_t kind) noexcept {
    if (kind != kKindTruck) return Model::player;
    if (!key || !*key) return Model::unknown;
    if (std::strcmp(key, "truck_npc") == 0) return Model::celd;
    return Model::player;
}

// What the game says about one vehicle (unread fields keep their defaults).
struct UnitFacts {
    std::uint8_t kind = 0;               // pool kind
    Model model = Model::unknown;
    std::uint32_t rider = kNoActor;      // unit +0x158
    std::uint8_t b19d = 0;               // unit +0x19D
    bool ent = false;                    // the entity's seat was read
    std::uint32_t ent_has = 0;           // entity +0x63C
    std::uint32_t ent_driver = kNoActor; // entity +0x8B0
};

enum class Class : std::uint8_t {
    player = 0,         // a player's vehicle (a player or nobody drives it)
    player_npc,         // a player's vehicle a non-player drives
    mule_empty,         // a MULE truck nobody drives
    mule_driven,        // a MULE truck a non-player (a MULE) drives
    mule_player,        // a MULE truck a player drives (taken)
    unknown_model,      // a truck whose model could not be read
};
inline const char* ClassName(Class c) noexcept {
    switch (c) {
        case Class::player: return "player vehicle";
        case Class::player_npc: return "player vehicle NPC-driven";
        case Class::mule_empty: return "MULE truck empty";
        case Class::mule_driven: return "MULE truck MULE-driven";
        case Class::mule_player: return "MULE truck player-driven";
        case Class::unknown_model: return "truck model unknown";
    }
    return "?";
}

struct Facts {
    Class cls = Class::player;
    Driver driver = Driver::none;
    std::uint32_t driver_id = kNoActor;
    bool last_npc = false;   // +0x19D bit1 (the last driver was a non-player) with nobody driving now
    bool follower = false;   // seated with driver 0x1FFFFF: the game's NPC controller (the mod's follower)
    bool agree = true;       // the unit's rider, its +0x19D bits and the entity's seat say the same
};

// The driver is the unit's rider; with none there the entity's seat (seated with an id) is used. "MULE-driven" in full is a
// type-1 rider, +0x19D bit1, +0x63C = 1 and +0x8B0 = the rider (`agree`).
inline Facts Classify(const UnitFacts& u) noexcept {
    Facts f;
    std::uint32_t id = u.rider < kNoActor ? u.rider : kNoActor;
    const bool seated = u.ent && u.ent_has == 1;
    if (id == kNoActor && seated && u.ent_driver < kNoActor) id = u.ent_driver;
    f.driver_id = id;
    f.driver = DriverOf(id);
    f.follower = seated && u.ent_driver >= kNoActor && u.rider >= kNoActor;
    f.last_npc = f.driver == Driver::none && (u.b19d & 2u) != 0;
    if (f.driver != Driver::none) {
        const bool bit_ok = f.driver == Driver::player ? (u.b19d & 3u) == 1u : (u.b19d & 3u) == 2u;
        const bool seat_ok = !u.ent || (u.ent_has == 1 && u.ent_driver == id);
        f.agree = u.rider == id && bit_ok && seat_ok;
    } else {
        f.agree = !u.ent || u.ent_has == 0 || f.follower;
    }
    if (u.kind == kKindTruck && u.model == Model::celd)
        f.cls = f.driver == Driver::none ? Class::mule_empty : f.driver == Driver::player ? Class::mule_player : Class::mule_driven;
    else if (u.kind == kKindTruck && u.model == Model::unknown)
        f.cls = Class::unknown_model;
    else
        f.cls = NonPlayer(f.driver) ? Class::player_npc : Class::player;
    return f;
}

// ---------------------------------------------------------------- the hold
enum class HoldWhy : std::uint8_t { none = 0, npc_here, npc_there, celd_here, celd_there };
inline const char* HoldWhyName(HoldWhy w) noexcept {
    switch (w) {
        case HoldWhy::none: return "shared";
        case HoldWhy::npc_here: return "a non-player drives it here";
        case HoldWhy::npc_there: return "a non-player drives it in the other game";
        case HoldWhy::celd_here: return "a MULE truck no player drives";
        case HoldWhy::celd_there: return "a MULE truck in the other game, no player drives it";
    }
    return "?";
}

struct HoldIn {
    bool on = false;                     // vehicle_mule_hold and the truck byte checks
    bool followed = false;               // the partner drives it (vehicle_drive moves it here as a follower)
    bool have_here = false;
    Driver here_driver = Driver::none;
    Model here_model = Model::unknown;
    bool have_there = false;             // fresh facts from the other game
    Driver there_driver = Driver::none;
    Model there_model = Model::unknown;
    bool player_last = false;            // review: a player drove it last (PlayerLast: seen, no non-player since)
};
// First match wins: off; the partner drives it; a player drives it in either game (a taken MULE truck stays under the
// shared vehicles' rules); a non-player drives it here; there; a player drove it last (review: it stays shared, so its final
// placement and PARK frames still go out and both games agree where the player left it); a MULE truck (here, else there).
inline HoldWhy Hold(const HoldIn& in) noexcept {
    if (!in.on || in.followed) return HoldWhy::none;
    if ((in.have_here && in.here_driver == Driver::player) || (in.have_there && in.there_driver == Driver::player)) return HoldWhy::none;
    if (in.have_here && NonPlayer(in.here_driver)) return HoldWhy::npc_here;
    if (in.have_there && NonPlayer(in.there_driver)) return HoldWhy::npc_there;
    if (in.player_last) return HoldWhy::none;
    if (in.have_here && in.here_model == Model::celd) return HoldWhy::celd_here;
    if (in.have_there && in.there_model == Model::celd) return HoldWhy::celd_there;
    return HoldWhy::none;
}
// A player in either game (the immediate release).
inline bool PlayerDrives(const HoldIn& in) noexcept {
    return (in.have_here && in.here_driver == Driver::player) || (in.have_there && in.there_driver == Driver::player);
}
// Round 23 review: whether a player drove this truck last, from what was seen (`was`: the previous poll's answer). Set while a
// player drives it in either game (here, or the other game's facts); cleared when a non-player drives it in either game; kept
// otherwise (nobody drives it). Not from the sticky +0x19D bits: both games must agree, and each sees the player through its
// own poll or the other's facts. Forgotten with the truck's hold state (gone from both games, a world or link change): it is
// then a MULE truck again (both games load the same place).
inline bool PlayerLast(bool was, const HoldIn& in) noexcept {
    if (PlayerDrives(in)) return true;
    if ((in.have_here && NonPlayer(in.here_driver)) || (in.have_there && NonPlayer(in.there_driver))) return false;
    return was;
}

// One truck's hold with the release latch: a reason holds it at once; without one it is released kHoldReleaseMs after the
// last reason, or at once when a player drives it (or the feature went off / the partner drives it).
class HoldLatch {
public:
    // Returns the hold in effect now; `changed` when it went on or off.
    HoldWhy Step(HoldWhy raw, bool immediate_release, double now_ms, bool* changed = nullptr) noexcept {
        const HoldWhy before = cur_;
        if (raw != HoldWhy::none) {
            cur_ = raw;
            last_ms_ = now_ms;
        } else if (cur_ != HoldWhy::none && (immediate_release || now_ms - last_ms_ >= kHoldReleaseMs)) {
            cur_ = HoldWhy::none;
        }
        if (changed) *changed = (before == HoldWhy::none) != (cur_ == HoldWhy::none);
        return cur_;
    }
    HoldWhy Current() const noexcept { return cur_; }
    bool Held() const noexcept { return cur_ != HoldWhy::none; }

private:
    HoldWhy cur_ = HoldWhy::none;
    double last_ms_ = 0;
};

// ---------------------------------------------------------------- the facts message ("STK1")
inline constexpr std::uint8_t kFactLastNpc = 1, kFactAgree = 2, kFactFollower = 4, kFactAllFlags = 7;
struct Fact {
    std::uint32_t host_id = 0;
    Model model = Model::unknown;
    Driver driver = Driver::none;
    std::uint8_t flags = 0;
    std::uint8_t seat = kNoSeat;
    std::uint32_t driver_id = kNoActor;
};
struct FactsMsg {
    std::uint8_t from = 0;      // 1 host, 2 guest
    std::uint16_t world = 0;    // the host's world counter as the sender knows it (low 16 bits)
    std::uint8_t n = 0;
    Fact e[kMaxFacts];
};

namespace detail {
inline void Put32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v), p[1] = static_cast<std::uint8_t>(v >> 8), p[2] = static_cast<std::uint8_t>(v >> 16),
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
inline std::uint32_t Get32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
           static_cast<std::uint32_t>(p[3]) << 24;
}
}  // namespace detail

inline std::size_t FactsSize(std::size_t n) noexcept { return kFactsHeader + n * kFactSize; }

// One fact is valid: a host id (1..0x1FFFFE), a known model and driver, the driver consistent with its id, known flags.
inline bool FactValid(const Fact& f) noexcept {
    if (f.host_id == 0 || f.host_id >= kNoActor) return false;
    if (static_cast<std::uint8_t>(f.model) > 2 || static_cast<std::uint8_t>(f.driver) > 3) return false;
    if (f.driver_id > kNoActor || DriverOf(f.driver_id) != f.driver) return false;
    return (f.flags & ~kFactAllFlags) == 0;
}

// The body (after the SVM1 header). 0 when it does not validate or does not fit.
inline std::size_t EncodeFacts(const FactsMsg& m, std::uint8_t* out, std::size_t cap) noexcept {
    if ((m.from != 1 && m.from != 2) || m.n > kMaxFacts || !out || cap < FactsSize(m.n)) return 0;
    for (std::size_t i = 0; i < m.n; ++i)
        if (!FactValid(m.e[i])) return 0;
    std::memcpy(out, "STK1", 4);
    out[4] = m.from, out[5] = m.n;
    out[6] = static_cast<std::uint8_t>(m.world), out[7] = static_cast<std::uint8_t>(m.world >> 8);
    std::uint8_t* p = out + kFactsHeader;
    for (std::size_t i = 0; i < m.n; ++i, p += kFactSize) {
        const Fact& f = m.e[i];
        detail::Put32(p, f.host_id);
        p[4] = static_cast<std::uint8_t>(f.model), p[5] = static_cast<std::uint8_t>(f.driver), p[6] = f.flags, p[7] = f.seat;
        detail::Put32(p + 8, f.driver_id);
    }
    return FactsSize(m.n);
}

// Strict: the magic, the sender, the exact size, every fact valid, no host id twice.
inline bool DecodeFacts(const std::uint8_t* in, std::size_t size, FactsMsg& m) noexcept {
    if (!in || size < kFactsHeader || std::memcmp(in, "STK1", 4) != 0) return false;
    if ((in[4] != 1 && in[4] != 2) || in[5] > kMaxFacts || size != FactsSize(in[5])) return false;
    FactsMsg t;
    t.from = in[4], t.n = in[5];
    t.world = static_cast<std::uint16_t>(in[6] | in[7] << 8);
    const std::uint8_t* p = in + kFactsHeader;
    for (std::size_t i = 0; i < t.n; ++i, p += kFactSize) {
        Fact& f = t.e[i];
        f.host_id = detail::Get32(p);
        if (p[4] > 2 || p[5] > 3) return false;
        f.model = static_cast<Model>(p[4]), f.driver = static_cast<Driver>(p[5]), f.flags = p[6], f.seat = p[7];
        f.driver_id = detail::Get32(p + 8);
        if (!FactValid(f)) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (t.e[j].host_id == f.host_id) return false;
    }
    m = t;
    return true;
}

// A fact from Classify (the seat from the enemy module, kNoSeat when unknown).
inline Fact FactOf(std::uint32_t host_id, Model model, const Facts& f, std::uint8_t seat) noexcept {
    Fact r;
    r.host_id = host_id, r.model = model, r.driver = f.driver, r.driver_id = f.driver_id, r.seat = seat;
    r.flags = static_cast<std::uint8_t>((f.last_npc ? kFactLastNpc : 0) | (f.agree ? kFactAgree : 0) | (f.follower ? kFactFollower : 0));
    return r;
}

// Whether a fact worth sending changed (model, driver, driver id; the seat and flags ride along).
inline bool FactChanged(const Fact& a, const Fact& b) noexcept {
    return a.model != b.model || a.driver != b.driver || a.driver_id != b.driver_id;
}

}  // namespace st::trucks
