// Shared structures (world sync, docs/world-sync.md): the decision rules, pure. The construction point type table
// (EDSConstructionPointType, read from ds.exe's RTTI enum; cross-checked against the game's own TypeToCategory /
// TypeToNetType at start-up), which structures are synced, the join binding, the guest build check, the removal
// classification, the online gate's verdict and the message dedup window. No game memory, no Windows: unit-tested in
// tests/world_rules_tests.cpp.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace st::wrules {

// ---------------------------------------------------------------- types
// Category (cp+0x35) and net type (cp+0xB2: 0 Stage, 1 Player, 2 Net) of each EDSConstructionPointType 0..45.
struct TypeRow {
    std::uint8_t category;
    std::uint8_t net;
    const char* name;
};
inline constexpr TypeRow kTypes[46] = {
    {0, 0, "delivery base"},      {2, 0, "prepper shelter"},    {1, 0, "safe house"},        {1, 1, "safe house"},
    {1, 2, "safe house"},         {3, 0, "postbox"},            {3, 1, "postbox"},           {3, 2, "postbox"},
    {4, 0, "watchtower"},         {4, 1, "watchtower"},         {4, 2, "watchtower"},        {5, 0, "catapult"},
    {5, 1, "catapult"},           {5, 2, "catapult"},           {6, 0, "generator"},         {6, 1, "generator"},
    {6, 2, "generator"},          {7, 0, "timefall shelter"},   {7, 1, "timefall shelter"},  {7, 2, "timefall shelter"},
    {8, 0, "MULE post"},          {9, 0, "zipline"},            {9, 1, "zipline"},           {9, 2, "zipline"},
    {10, 0, "ladder"},            {10, 1, "ladder"},            {10, 2, "ladder"},           {11, 0, "climbing rope"},
    {11, 1, "climbing rope"},     {11, 2, "climbing rope"},     {13, 0, "bridge 30 m"},      {13, 1, "bridge 30 m"},
    {13, 2, "bridge 30 m"},       {12, 0, "bridge 45 m"},       {12, 1, "bridge 45 m"},      {12, 2, "bridge 45 m"},
    {14, 0, "auto-paver"},        {15, 1, "cargo pod"},         {15, 0, "cargo pod"},        {15, 2, "cargo pod"},
    {16, 0, "chiral bridge"},     {16, 1, "chiral bridge"},     {16, 2, "chiral bridge"},    {17, 0, "jump ramp"},
    {17, 1, "jump ramp"},         {17, 2, "jump ramp"},
};
inline constexpr std::uint8_t kTypeCount = 46;
inline constexpr std::uint8_t kInvalidType = 46;  // the PCC preview's "cannot build here"
inline bool TypeKnown(int t) noexcept { return t >= 0 && t < kTypeCount; }
inline int CategoryOf(int t) noexcept { return TypeKnown(t) ? kTypes[t].category : -1; }
inline int NetOf(int t) noexcept { return TypeKnown(t) ? kTypes[t].net : -1; }
inline const char* TypeName(int t) noexcept { return TypeKnown(t) ? kTypes[t].name : "?"; }

// The W1 synced categories: the PCC structures (safe house, postbox, watchtower, catapult, generator, timefall shelter,
// bridges 45 m / 30 m, chiral bridge, jump ramp). W3: ladders / ropes (10 / 11). W4: ziplines (9). Not: the cargo pod
// (15), the auto-paver (14: the highway system), delivery bases, shelters, MULE posts.
inline bool SyncedCategory(int cat, bool ladders, bool ziplines = false) noexcept {
    switch (cat) {
        case 1: case 3: case 4: case 5: case 6: case 7: case 12: case 13: case 16: case 17: return true;
        case 10: case 11: return ladders;
        case 9: return ziplines;
        default: return false;
    }
}
// A Player-type structure of a synced category: the ones the mod creates, removes and binds.
inline bool IsSynced(int type, bool ladders = false, bool ziplines = false) noexcept {
    return NetOf(type) == 1 && SyncedCategory(CategoryOf(type), ladders, ziplines);
}
// W4: a zipline (category 9: DSZiplineConstructionPoint, types 21 / 22 / 23). Its record carries one subclass byte (+0x578
// <- cp+0x520); its connections are NOT in the record: the zipline's activation (vt+0x10 0x142CE5520) links it both ways to
// every zipline of its area within the config's range (+0x4AC, 0x142CAA000), so the same set of ziplines links the same in
// both worlds. A PCC structure: the printer, levels to contribute to.
inline bool Zipline(int type) noexcept { return CategoryOf(type) == 9; }
// Ladders and ropes get no printer (the game's own commit skips SetLastBuilt for them).
inline bool HasPrinter(int type) noexcept {
    const int c = CategoryOf(type);
    return c != 10 && c != 11;
}
// W3: a ladder (category 10: DSLadder, types 24 / 25 / 26) or a climbing anchor with its rope (category 11:
// DSFieldRopeConstructionPoint, types 27 / 28 / 29). Placed from the player's equipment (the same commit 0x1428D8070 and
// EnqueueInfo as the PCC), no printer, no online registration by the game, no levels to contribute to; picked up again by
// the player (the item comes back), never dismantled through the structure menu.
inline bool LadderOrRope(int type) noexcept {
    const int c = CategoryOf(type);
    return c == 10 || c == 11;
}

// W2: the structures whose level, materials and durability are shared. The synced Player structures, and the Stage
// (story) structures of the same categories: both worlds hold those from the same save with the same ids, so only their
// numbers are synced (never created or removed by the mod). Not: roads (the auto-paver, category 14: the highway system
// and its own uploads, W4), Net structures (each world keeps its own online ones), ladders / ropes (no levels).
// W4: ziplines too when that part is shared (`ziplines`).
inline bool Contributable(int type, bool ziplines = false) noexcept {
    if (IsSynced(type, false, ziplines)) return true;
    return NetOf(type) == 0 && SyncedCategory(CategoryOf(type), false, ziplines);
}
// W4: the structures whose durability the host's DUR carries: the contributable ones, and (W3 gap closed) the shared ladders
// and ropes, which decay in timefall like the rest (each world used to decay its own copy).
inline bool DurShared(int type, bool ladders, bool ziplines) noexcept {
    return Contributable(type, ziplines) || (ladders && NetOf(type) == 1 && LadderOrRope(type));
}

// ---------------------------------------------------------------- identity
inline constexpr std::uint32_t kPlayerIdBase = 10000;
inline constexpr std::uint32_t kNetIdBase = 100000;
inline bool PlayerId(std::uint32_t id) noexcept { return id >= kPlayerIdBase + 1 && id < kNetIdBase; }
// The guard: same type, positions within `tol` m (0.5 m; ladders / ropes 0.3 m: the game re-attaches a ladder to the ground
// when it realizes in each world (0x142D2E5B0, a ray cast), which moved one by 1 cm between the two worlds in run
// 20261121-175940; 0.3 m still tells two ladders apart, they are never placed that close).
inline double GuardTol(int type) noexcept {
    const int c = CategoryOf(type);
    return (c == 10 || c == 11) ? 0.3 : 0.5;
}
inline double Dist(const double (&a)[3], const double (&b)[3]) noexcept {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline bool GuardOk(int type_a, const double (&pos_a)[3], int type_b, const double (&pos_b)[3]) noexcept {
    return type_a == type_b && Dist(pos_a, pos_b) <= GuardTol(type_a);
}
// The guest's Player id counter after mirroring host id `hid` exactly (NewId 0x142C5BD80: ++counter + 10000).
inline std::int32_t RaisedCounter(std::int32_t cur, std::uint32_t hid) noexcept {
    const std::int64_t need = static_cast<std::int64_t>(hid) - kPlayerIdBase;
    return need > cur ? static_cast<std::int32_t>(need) : cur;
}

// The join binding of one host entry against the guest's world (same id first).
enum class Bind : std::uint8_t {
    bound,         // the guest has it: same id, type, place
    replace,       // the guest's CP with that id is something else: remove it, then mirror the host's
    mirror,        // the guest lacks it: NEED
};
inline Bind BindEntry(bool guest_has_id, int guest_type, const double (&guest_pos)[3], int host_type, const double (&host_pos)[3]) noexcept {
    if (!guest_has_id) return Bind::mirror;
    return GuardOk(guest_type, guest_pos, host_type, host_pos) ? Bind::bound : Bind::replace;
}

// ---------------------------------------------------------------- the guest's build, at the host
enum class GuestBuild : std::uint8_t { ok, invalid, type, disabled, too_far, not_ready, host_real };
struct GuestBuildIn {
    bool record_valid = false;
    bool host_ready = false;
    int type = -1;
    bool ladders = false;           // the W3 part is on
    bool ziplines = false;          // the W4 part is on
    bool has_pcc = false;
    double place[3] = {};           // the structure's position
    double pcc[3] = {};             // the printer (PCC) place, when has_pcc
    double body[3] = {};            // where the host sees the guest's body (its STD1 / pose), or the guest's own report
    bool dev = false;               // a dev build
    bool host_allows_dev = false;   // the host's own dev rules (its saves sandboxed)
};
inline constexpr double kGuestBuildMaxM = 60.0;
inline GuestBuild ValidateGuestBuild(const GuestBuildIn& in) noexcept {
    if (!in.record_valid) return GuestBuild::invalid;
    if (!in.host_ready) return GuestBuild::not_ready;
    if (NetOf(in.type) != 1) return GuestBuild::type;
    if (!IsSynced(in.type, in.ladders, in.ziplines)) return GuestBuild::disabled;
    if (in.dev && !in.host_allows_dev) return GuestBuild::host_real;
    // a dev build is placed by the action, not by her body; a real one is within reach of her body
    if (!in.dev && Dist(in.place, in.body) > kGuestBuildMaxM) return GuestBuild::too_far;
    if (!in.dev && in.has_pcc && Dist(in.pcc, in.body) > kGuestBuildMaxM) return GuestBuild::too_far;
    return GuestBuild::ok;
}

// ---------------------------------------------------------------- removals (the host's census)
enum class Removal : std::uint8_t { manual = 1, evicted = 2, decay = 3, area = 4, pickup = 5 };
// `manual_bit` cp+0xAD & 0x80 (RemoveManually), `noted_manual` our RemoveManually detour saw it, `durability` cp+0xD8,
// `same_category_added` a CP of that category appeared in the same census tick (the game's per-category limit evicted it).
// W3: a ladder or rope that goes with durability left and no other cause was picked up by its player (the game's pick-up
// takes it back into the inventory; it is neither RemoveManually nor a decay).
inline Removal ClassifyRemoval(bool manual_bit, bool noted_manual, float durability, bool same_category_added, bool ladder_or_rope = false) noexcept {
    if (manual_bit || noted_manual) return Removal::manual;
    if (same_category_added) return Removal::evicted;
    if (durability <= 0.0f) return Removal::decay;
    if (ladder_or_rope) return Removal::pickup;
    return Removal::area;
}
// W3, the guest: a bound structure vanished from her world without the host's REM. A ladder or rope with durability left
// was picked up by her (the game gave her the item back): the host removes its copy (REMREQ pickup). Anything else (a PCC
// structure, or a ladder / rope that decayed in her world first) is the host's to keep: LOST (the host sends ADD again).
enum class Vanished : std::uint8_t { lost, pickup };
inline Vanished GuestVanished(int type, float last_durability) noexcept {
    return LadderOrRope(type) && last_durability > 0.0f ? Vanished::pickup : Vanished::lost;
}
// round 5: a vanish is only taken as her pick-up (which removes the host's REAL ladder / rope) when everything around it
// looks like one: the census saw a world (not an empty or emptied live list), at most kPickupsPerTick bound ladders / ropes
// went in that census tick (a pick-up is one structure with its own animation; a mass vanish is the game's), and her body
// was within kPickupReachM of it (a pick-up needs her next to it; an area clear elsewhere does not). Anything else is LOST:
// the host keeps its structure and sends it again.
inline constexpr unsigned kPickupsPerTick = 2;
inline constexpr double kPickupReachM = 20.0;
struct PickupIn {
    bool census_has_world = false;  // the census read a non-empty live list
    unsigned vanished_lr = 0;       // bound ladders / ropes that vanished in this census tick (candidates)
    bool have_body = false;         // her body's position is known
    double dist_m = 1e9;            // her body to the structure's last position
};
inline bool PickupPlausible(const PickupIn& in) noexcept {
    return in.census_has_world && in.vanished_lr >= 1 && in.vanished_lr <= kPickupsPerTick && in.have_body && in.dist_m <= kPickupReachM;
}
// round 5: a mirror her world keeps removing (its ground re-check, an area not realized): LOST at most kLostMax times per
// structure and world, then she gives it up (one log line); the join check's heal backs off while a heal changes nothing.
inline constexpr unsigned kLostMax = 3;
inline bool LostGiveUp(unsigned lost_sent) noexcept { return lost_sent >= kLostMax; }
inline double HealGapMs(double base_ms, unsigned idle_heals) noexcept {
    const unsigned k = idle_heals > 5 ? 5 : idle_heals;
    return base_ms * static_cast<double>(1u << k);
}

// ---------------------------------------------------------------- contributions (W2)
// The game's own building blocks (ds.exe, decompiled):
//   AddOwn 0x142C41540(cp, m[6])      cp+0x210 += m; NEVER writes the level (+0x84); telemetry, stats, likes (non-own only)
//   AddOthers 0x142C321A0(cp, stamp, m[6], force, road)  cp+0x248 += m while level < max; nothing online
//   Invested 0x142C58A50              own (+0x210) + the contributor list (+0x228) + others (+0x248)
//   NeedToLevel 0x142C5D9D0(cp, L)    the materials levels 1..L need in all (cumulative)
//   Status 0x142C59770                0 enough for the next level, 1 missing, 2 max level, 3 no level-up, 4 a level-up runs
//   QueueLevelUp 0x142C69170          the id into mgr+0x56A2A8; the manager update calls vt+0xC0 StartLevelUp(cp, the level
//                                     the materials reach): the build-up / printer (level 0) and the level-up animation
// Materials are cumulative and never consumed: two worlds with the same totals reach the same level.
struct Clamp {
    std::int32_t applied[6] = {};
    std::int32_t excess[6] = {};
    bool any_applied = false, any_excess = false;
};
// The guest's contribution against what the structure still needs for its LAST level (cumulative need - invested): what
// is not needed (it was at its last level, or the host's own contribution came first) is the excess.
inline Clamp ClampContribution(const std::int32_t (&m)[6], const std::int32_t (&need_total)[6], const std::int32_t (&invested)[6]) noexcept {
    Clamp c;
    for (int i = 0; i < 6; ++i) {
        std::int64_t room = static_cast<std::int64_t>(need_total[i]) - invested[i];
        if (room < 0) room = 0;
        const std::int32_t v = m[i] < 0 ? 0 : m[i];
        c.applied[i] = static_cast<std::int32_t>(v < room ? v : room);
        c.excess[i] = v - c.applied[i];
        if (c.applied[i] > 0) c.any_applied = true;
        if (c.excess[i] > 0) c.any_excess = true;
    }
    return c;
}
// The host queues a level-up after applying the guest's contribution (and re-checks after a level-up ends, so materials
// beyond the next level count toward the one after, as the game's own terminal would): the game's Status says enough,
// none runs (Status 4 covers a running one), none waits in the queue.
inline bool HostShouldQueueLevelUp(int status, bool running, bool queued) noexcept { return status == 0 && !running && !queued; }
// The guest follows the host's level-up: the host is ahead (or has one queued / running), hers neither runs nor waits, and
// her materials (written from the host's) reach a higher level than she has.
inline bool GuestShouldQueueLevelUp(int host_level, int host_levelup, int guest_level, bool guest_running, bool guest_queued, int guest_reached) noexcept {
    if (guest_running || guest_queued) return false;
    if (!(host_level > guest_level || host_levelup != 0)) return false;
    return guest_reached > guest_level;
}
// The guest's others' sum (+0x248) so that her Invested equals the host's: invested - own - her contributor list (the list
// came from the same save; a difference there lands in "others").
inline void OthersFor(const std::int32_t (&invested)[6], const std::int32_t (&own)[6], const std::int32_t (&list)[6], std::int32_t (&out)[6]) noexcept {
    for (int i = 0; i < 6; ++i) {
        const std::int64_t v = static_cast<std::int64_t>(invested[i]) - own[i] - list[i];
        out[i] = v < 0 ? 0 : static_cast<std::int32_t>(v);
    }
}
// Durability changes worth a DUR (the game decays it continuously in timefall): 2 % of the last sent, below 10 % of the
// level's maximum, or a crossing of zero.
inline bool DurWorthSending(float last_sent, float now, float max) noexcept {
    if ((last_sent > 0) != (now > 0)) return true;
    const float d = now > last_sent ? now - last_sent : last_sent - now;
    const float base = last_sent > 0 ? last_sent : 1.0f;
    if (d > 0.02f * base) return true;
    return max > 0 && now < 0.1f * max && d > 0.5f;
}

// A small set of applied request ids (the guest's MATS, across link sessions).
class ReqLog {
public:
    void Reset() noexcept { n_ = 0, next_ = 0; }
    bool Seen(std::uint32_t nonce, std::uint16_t k) const noexcept {
        for (unsigned i = 0; i < n_; ++i)
            if (ids_[i].nonce == nonce && ids_[i].n == k) return true;
        return false;
    }
    void Add(std::uint32_t nonce, std::uint16_t k) noexcept {
        ids_[next_] = {nonce, k};
        next_ = (next_ + 1) % kCap;
        if (n_ < kCap) ++n_;
    }

private:
    struct Id {
        std::uint32_t nonce;
        std::uint16_t n;
    };
    static constexpr unsigned kCap = 512;
    Id ids_[kCap] = {};
    unsigned n_ = 0, next_ = 0;
};

// ---------------------------------------------------------------- the online gate
// Whether an online call (RegisterNew, the removal drain, telemetry, the materials upload) is dropped.
struct GateIn {
    int role = 0;              // 0 solo, 1 host, 2 guest
    bool sync_on = false;      // world sync on in this game
    bool automation = false;   // a hidden run: drop everything, both games
    bool cp_known = false;     // the call names a construction point of the manager's pool
    bool cp_offline = false;   // its cp+0xAB bit 7
    bool cp_mod = false;       // the mod created it, or a removal the mod applied
    bool mirroring = false;    // inside the mod's own game call (thread-local)
    bool guest_locked = false; // the gate went in for a world-sync guest: her world is a copy of the host's save for the
                               // whole session, even after world sync stopped (a fault, the co-op session ending)
};
inline bool GateDrops(const GateIn& g) noexcept {
    if (g.automation) return true;
    if (g.role == 2 && g.guest_locked) return true;  // never reopened by a stop: her copies carry the host's server ids
    if (!g.sync_on) return false;
    if (g.role == 2) return true;  // the guest's world is a copy of the host's save: it never talks to the server
    if (g.mirroring) return true;
    if (g.cp_known && (g.cp_offline || g.cp_mod)) return true;
    return false;
}
// The OTGC request names that write to the game server. The game names its requests "urn:otgc_<verb>_<object>" (run
// 20261121-150217-world-census: urn:otgc_find_qpid_objects, urn:otgc_lookup, urn:otgc_get_highway_resources, ...); the
// endpoint strings are "otgc://gateway/createObject". A name counts as a write when its verb (after the last ':' or '/',
// without "otgc_") starts with a writing verb: create, update, delete, set, send, devote, add, remove, post, put, upload,
// register, like (both spellings). Everything else (get, find, lookup, reverse_lookup, refresh, ...) is a read.
inline bool OtgcWriteName(const char* n) noexcept {
    if (!n || !n[0]) return false;
    const char* last = n;
    for (const char* p = n; *p; ++p)
        if (*p == '/' || *p == ':') last = p + 1;
    if (last[0] == 'o' && last[1] == 't' && last[2] == 'g' && last[3] == 'c' && last[4] == '_') last += 5;
    static constexpr const char* kVerbs[] = {"create", "update", "delete", "set", "send", "devote", "add", "remove", "post", "put", "upload", "register", "like"};
    for (const char* v : kVerbs) {
        std::size_t i = 0;
        while (v[i] && last[i] == v[i]) ++i;
        if (v[i]) continue;
        // the verb must end there: "_" (snake case), an upper-case letter (camel case) or the end
        const char c = last[i];
        if (c == 0 || c == '_' || (c >= 'A' && c <= 'Z')) return true;
    }
    return false;
}

// ---------------------------------------------------------------- dedup of re-sent messages across link sessions
class SeqWindow {
public:
    void Reset() noexcept {
        for (auto& s : seen_) s = 0;
        next_ = 0;
    }
    // true the first time `seq` is seen (among the last 256)
    bool Fresh(std::uint32_t seq) noexcept {
        if (!seq) return false;
        for (std::uint32_t s : seen_)
            if (s == seq) return false;
        seen_[next_] = seq;
        next_ = (next_ + 1) & 255u;
        return true;
    }

private:
    std::uint32_t seen_[256] = {};
    unsigned next_ = 0;
};
// round 5: the window belongs to one sender process. Every game numbers its messages from 1, so a partner that restarted
// (or a new host) would have its first 256 messages taken for repeats of the old process's: its HELLO carries a random
// per-process nonce, and a different one starts a fresh window. The same process keeps its window across link sessions.
class SenderWindow {
public:
    // true when `nonce` is a different sender than the one before (the window starts again); 0 = unknown, ignored
    bool Sender(std::uint32_t nonce) noexcept {
        if (!nonce || nonce == sender_) return false;
        sender_ = nonce;
        win_.Reset();
        return true;
    }
    bool Fresh(std::uint32_t seq) noexcept { return win_.Fresh(seq); }
    std::uint32_t sender() const noexcept { return sender_; }

private:
    SeqWindow win_;
    std::uint32_t sender_ = 0;
};

// A position key for an idempotent guest build: type + position rounded to 0.25 m.
struct PosKey {
    std::uint8_t type = 0;
    std::int32_t x = 0, y = 0, z = 0;
    bool operator==(const PosKey& o) const noexcept { return type == o.type && x == o.x && y == o.y && z == o.z; }
};
inline PosKey MakePosKey(int type, const double (&p)[3]) noexcept {
    PosKey k;
    k.type = static_cast<std::uint8_t>(type);
    k.x = static_cast<std::int32_t>(std::llround(p[0] * 4.0));
    k.y = static_cast<std::int32_t>(std::llround(p[1] * 4.0));
    k.z = static_cast<std::int32_t>(std::llround(p[2] * 4.0));
    return k;
}

}  // namespace st::wrules
