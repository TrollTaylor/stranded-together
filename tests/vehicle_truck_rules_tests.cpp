// Round 23 D (native/mod/vehicle_truck_rules.hpp): the truck classifier, the hold decision and its latch, the STK1 facts codec.
#include <cstdio>
#include <cstring>

#include "config_values.hpp"
#include "vehicle_truck_rules.hpp"

using namespace st;
using namespace st::trucks;

namespace {
int g_checks = 0, g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

constexpr std::uint32_t kMule = 0x2000u | 0x15;   // type 1
constexpr std::uint32_t kSam = 0x0001u;           // type 0
constexpr std::uint32_t kOther = 0x4000u | 0x3;   // type 2

UnitFacts Truck(Model m, std::uint32_t rider, std::uint8_t b19d, bool ent, std::uint32_t has, std::uint32_t drv) {
    UnitFacts u;
    u.kind = kKindTruck, u.model = m, u.rider = rider, u.b19d = b19d, u.ent = ent, u.ent_has = has, u.ent_driver = drv;
    return u;
}

void TestActorTypes() {
    CHECK(ActorType(kMule) == 1 && ActorType(kSam) == 0 && ActorType(kOther) == 2);
    CHECK(DriverOf(kNoActor) == Driver::none && DriverOf(0xFFFFFFFFu) == Driver::none);
    CHECK(DriverOf(kSam) == Driver::player && DriverOf(kMule) == Driver::mule && DriverOf(kOther) == Driver::other);
    CHECK(NonPlayer(Driver::mule) && NonPlayer(Driver::other) && !NonPlayer(Driver::player) && !NonPlayer(Driver::none));
    CHECK(ModelOfKey("truck_npc", 2) == Model::celd && ModelOfKey("truck", 2) == Model::player);
    CHECK(ModelOfKey("", 2) == Model::unknown && ModelOfKey(nullptr, 2) == Model::unknown);
    CHECK(ModelOfKey("truck_npc", 3) == Model::player && ModelOfKey(nullptr, 1) == Model::player);
}

void TestClassify() {
    // a MULE truck its MULE drives: every source agrees
    Facts f = Classify(Truck(Model::celd, kMule, 2, true, 1, kMule));
    CHECK(f.cls == Class::mule_driven && f.driver == Driver::mule && f.driver_id == kMule && f.agree && !f.follower && !f.last_npc);
    // the +0x19D bits say a player drove last: disagree, still MULE-driven
    f = Classify(Truck(Model::celd, kMule, 1, true, 1, kMule));
    CHECK(f.cls == Class::mule_driven && !f.agree);
    // the entity's seat names another id
    f = Classify(Truck(Model::celd, kMule, 2, true, 1, kOther));
    CHECK(f.cls == Class::mule_driven && f.driver_id == kMule && !f.agree);
    // no entity read: the unit decides
    f = Classify(Truck(Model::celd, kMule, 2, false, 0, kNoActor));
    CHECK(f.cls == Class::mule_driven && f.agree);
    // an empty MULE truck; the last driver a MULE (sticky bit1)
    f = Classify(Truck(Model::celd, kNoActor, 2, true, 0, kNoActor));
    CHECK(f.cls == Class::mule_empty && f.driver == Driver::none && f.last_npc && f.agree);
    f = Classify(Truck(Model::celd, kNoActor, 0, true, 0, kNoActor));
    CHECK(f.cls == Class::mule_empty && !f.last_npc);
    // a taken MULE truck (a player drives it)
    f = Classify(Truck(Model::celd, kSam, 1, true, 1, kSam));
    CHECK(f.cls == Class::mule_player && f.driver == Driver::player && f.agree);
    // a player truck, empty / player / MULE
    f = Classify(Truck(Model::player, kNoActor, 1, true, 0, kNoActor));
    CHECK(f.cls == Class::player && f.driver == Driver::none && !f.last_npc);
    f = Classify(Truck(Model::player, kSam, 1, true, 1, kSam));
    CHECK(f.cls == Class::player && f.driver == Driver::player && f.agree);
    f = Classify(Truck(Model::player, kMule, 2, true, 1, kMule));
    CHECK(f.cls == Class::player_npc && f.driver == Driver::mule);
    // the mod's follower: seated, driver 0x1FFFFF, rider none: nobody drives, agree
    f = Classify(Truck(Model::player, kNoActor, 1, true, 1, kNoActor));
    CHECK(f.cls == Class::player && f.follower && f.driver == Driver::none && f.agree);
    // the rider none, the entity seated with an id: that id drives (the unit not written yet), disagree
    f = Classify(Truck(Model::celd, kNoActor, 0, true, 1, kMule));
    CHECK(f.cls == Class::mule_driven && f.driver_id == kMule && !f.agree);
    // a rider above 0x1FFFFF is none
    f = Classify(Truck(Model::celd, 0x00FFFFFFu, 0, false, 0, kNoActor));
    CHECK(f.cls == Class::mule_empty && f.driver == Driver::none);
    // a seat that says seated with nobody known, unit none, no follower contradiction: still agree
    f = Classify(Truck(Model::player, kNoActor, 0, true, 2, kNoActor));
    CHECK(!f.agree);
    // unknown model
    f = Classify(Truck(Model::unknown, kMule, 2, true, 1, kMule));
    CHECK(f.cls == Class::unknown_model && f.driver == Driver::mule);
    // other kinds are never MULE trucks
    UnitFacts t = Truck(Model::celd, kNoActor, 0, false, 0, kNoActor);
    t.kind = 3;
    CHECK(Classify(t).cls == Class::player);
    t.rider = kOther, t.b19d = 2;
    CHECK(Classify(t).cls == Class::player_npc && Classify(t).driver == Driver::other);
    for (int c = 0; c <= 5; ++c) CHECK(std::strcmp(ClassName(static_cast<Class>(c)), "?") != 0);
}

HoldIn In(Driver hd, Model hm, bool have_there = false, Driver td = Driver::none, Model tm = Model::unknown) {
    HoldIn in;
    in.on = true, in.have_here = true, in.here_driver = hd, in.here_model = hm;
    in.have_there = have_there, in.there_driver = td, in.there_model = tm;
    return in;
}

void TestHold() {
    CHECK(Hold(In(Driver::mule, Model::celd)) == HoldWhy::npc_here);
    CHECK(Hold(In(Driver::none, Model::celd)) == HoldWhy::celd_here);
    CHECK(Hold(In(Driver::none, Model::player)) == HoldWhy::none);
    CHECK(Hold(In(Driver::player, Model::player)) == HoldWhy::none);
    CHECK(Hold(In(Driver::mule, Model::player)) == HoldWhy::npc_here);   // a MULE in a player truck
    CHECK(Hold(In(Driver::other, Model::player)) == HoldWhy::npc_here);
    // the other game's MULE drives it
    CHECK(Hold(In(Driver::none, Model::player, true, Driver::mule, Model::player)) == HoldWhy::npc_there);
    CHECK(Hold(In(Driver::none, Model::unknown, true, Driver::none, Model::celd)) == HoldWhy::celd_there);
    // a player in either game: the shared rules (a taken MULE truck)
    CHECK(Hold(In(Driver::player, Model::celd)) == HoldWhy::none);
    CHECK(Hold(In(Driver::mule, Model::celd, true, Driver::player, Model::celd)) == HoldWhy::none);
    CHECK(Hold(In(Driver::none, Model::celd, true, Driver::player, Model::celd)) == HoldWhy::none);
    // here first, then there
    CHECK(Hold(In(Driver::mule, Model::celd, true, Driver::mule, Model::celd)) == HoldWhy::npc_here);
    CHECK(Hold(In(Driver::none, Model::celd, true, Driver::mule, Model::celd)) == HoldWhy::npc_there);
    // off; followed (the partner drives it)
    HoldIn in = In(Driver::mule, Model::celd);
    in.on = false;
    CHECK(Hold(in) == HoldWhy::none);
    in = In(Driver::none, Model::celd);
    in.followed = true;
    CHECK(Hold(in) == HoldWhy::none);
    // nothing known here, nothing there
    HoldIn empty;
    empty.on = true;
    CHECK(Hold(empty) == HoldWhy::none);
    CHECK(PlayerDrives(In(Driver::none, Model::celd, true, Driver::player, Model::celd)));
    CHECK(!PlayerDrives(In(Driver::mule, Model::celd)));
    for (int w = 0; w <= 4; ++w) CHECK(std::strcmp(HoldWhyName(static_cast<HoldWhy>(w)), "?") != 0);
}

// Review: a MULE truck a player drove last stays shared (its final placement and PARK frames go out) until a non-player drives it.
void TestPlayerLast() {
    // the host's Sam drives it (here), then gets out: no hold on the poll he gets out
    HoldIn in = In(Driver::player, Model::celd);
    bool last = PlayerLast(false, in);
    CHECK(last);
    in = In(Driver::none, Model::celd);
    last = PlayerLast(last, in);
    CHECK(last);
    in.player_last = last;
    CHECK(Hold(in) == HoldWhy::none);
    // the other game: the player seen through its facts, then nobody there
    HoldIn g = In(Driver::none, Model::celd, true, Driver::player, Model::celd);
    bool glast = PlayerLast(false, g);
    CHECK(glast);
    g = In(Driver::none, Model::celd, true, Driver::none, Model::celd);
    glast = PlayerLast(glast, g);
    g.player_last = glast;
    CHECK(glast && Hold(g) == HoldWhy::none);
    // a MULE gets in later (either game): held at once and the player's drive forgotten
    HoldIn m = In(Driver::mule, Model::celd);
    m.player_last = PlayerLast(true, m);
    CHECK(!m.player_last && Hold(m) == HoldWhy::npc_here);
    HoldIn mt = In(Driver::none, Model::celd, true, Driver::mule, Model::celd);
    mt.player_last = PlayerLast(true, mt);
    CHECK(!mt.player_last && Hold(mt) == HoldWhy::npc_there);
    // ... then the MULE gets out: a MULE truck again (held)
    HoldIn e = In(Driver::none, Model::celd);
    e.player_last = PlayerLast(m.player_last, e);
    CHECK(!e.player_last && Hold(e) == HoldWhy::celd_here);
    // never driven by a player: the usual hold; a non-player outranks the player's last drive even with the flag set
    HoldIn n = In(Driver::none, Model::celd);
    CHECK(!PlayerLast(false, n) && Hold(n) == HoldWhy::celd_here);
    HoldIn np = In(Driver::mule, Model::celd);
    np.player_last = true;
    CHECK(Hold(np) == HoldWhy::npc_here);
    // off or followed: shared whatever the flag
    HoldIn off = In(Driver::none, Model::celd);
    off.on = false;
    CHECK(Hold(off) == HoldWhy::none);
    // the latch: a truck held as a MULE truck, a player gets in (released at once), gets out: no new hold
    HoldLatch l;
    bool ch = false;
    CHECK(l.Step(Hold(In(Driver::none, Model::celd)), false, 0, &ch) == HoldWhy::celd_here && ch);
    HoldIn p = In(Driver::player, Model::celd);
    p.player_last = PlayerLast(false, p);
    CHECK(l.Step(Hold(p), PlayerDrives(p), 100, &ch) == HoldWhy::none && ch);
    HoldIn out = In(Driver::none, Model::celd);
    out.player_last = PlayerLast(p.player_last, out);
    CHECK(l.Step(Hold(out), PlayerDrives(out), 200, &ch) == HoldWhy::none && !ch);
}

void TestLatch() {
    HoldLatch l;
    bool ch = false;
    CHECK(l.Step(HoldWhy::none, false, 0, &ch) == HoldWhy::none && !ch);
    CHECK(l.Step(HoldWhy::npc_here, false, 100, &ch) == HoldWhy::npc_here && ch && l.Held());
    CHECK(l.Step(HoldWhy::celd_here, false, 200, &ch) == HoldWhy::celd_here && !ch);  // the reason changes, still held
    CHECK(l.Step(HoldWhy::none, false, 3000, &ch) == HoldWhy::celd_here && !ch);      // within 5 s of the last reason
    CHECK(l.Step(HoldWhy::none, false, 5199, &ch) == HoldWhy::celd_here && !ch);
    CHECK(l.Step(HoldWhy::none, false, 5200, &ch) == HoldWhy::none && ch && !l.Held());
    // a player: at once
    CHECK(l.Step(HoldWhy::npc_there, false, 6000, &ch) == HoldWhy::npc_there && ch);
    CHECK(l.Step(HoldWhy::none, true, 6100, &ch) == HoldWhy::none && ch);
    CHECK(l.Current() == HoldWhy::none);
}

void TestCodec() {
    FactsMsg m;
    m.from = 1, m.world = 0x1234, m.n = 3;
    m.e[0] = FactOf(0x20018, Model::celd, Classify(Truck(Model::celd, kMule, 2, true, 1, kMule)), 2);
    m.e[1] = FactOf(0x20019, Model::celd, Classify(Truck(Model::celd, kNoActor, 2, true, 0, kNoActor)), kNoSeat);
    m.e[2] = FactOf(0x2001A, Model::player, Classify(Truck(Model::player, kNoActor, 1, true, 1, kNoActor)), kNoSeat);
    CHECK(m.e[0].flags == kFactAgree && m.e[1].flags == (kFactLastNpc | kFactAgree) && m.e[2].flags == (kFactAgree | kFactFollower));
    std::uint8_t buf[1024];
    const std::size_t n = EncodeFacts(m, buf, sizeof buf);
    CHECK(n == 8 + 3 * 12 && std::memcmp(buf, "STK1", 4) == 0);
    FactsMsg d;
    CHECK(DecodeFacts(buf, n, d));
    CHECK(d.from == 1 && d.world == 0x1234 && d.n == 3);
    CHECK(d.e[0].host_id == 0x20018 && d.e[0].model == Model::celd && d.e[0].driver == Driver::mule && d.e[0].driver_id == kMule &&
          d.e[0].seat == 2);
    CHECK(d.e[1].driver == Driver::none && d.e[1].driver_id == kNoActor && d.e[1].seat == kNoSeat);
    // strictness
    CHECK(!DecodeFacts(buf, n - 1, d) && !DecodeFacts(buf, n + 1, d) && !DecodeFacts(buf, 4, d) && !DecodeFacts(nullptr, n, d));
    std::uint8_t bad[1024];
    auto Bad = [&](std::size_t at, std::uint8_t v) {
        std::memcpy(bad, buf, n);
        bad[at] = v;
        return !DecodeFacts(bad, n, d);
    };
    CHECK(Bad(0, 'X'));                  // magic
    CHECK(Bad(4, 3) && Bad(4, 0));       // sender
    CHECK(Bad(5, 4));                    // count vs size
    CHECK(Bad(8 + 4, 3));                // model
    CHECK(Bad(8 + 5, 4));                // driver kind
    CHECK(Bad(8 + 5, 1));                // driver kind inconsistent with the MULE id
    CHECK(Bad(8 + 6, 8));                // unknown flag
    CHECK(Bad(8 + 2, 0x20));             // host id >= 0x1FFFFF
    CHECK(Bad(8 + 12 + 0, 0x18));        // a host id twice (0x20018)
    {
        std::memcpy(bad, buf, n);
        std::memset(bad + 8, 0, 4);      // host id 0
        CHECK(!DecodeFacts(bad, n, d));
    }
    // encode refuses what decode would
    FactsMsg e = m;
    e.from = 0;
    CHECK(EncodeFacts(e, buf, sizeof buf) == 0);
    e = m;
    e.e[0].driver = Driver::player;      // inconsistent with the MULE id
    CHECK(EncodeFacts(e, buf, sizeof buf) == 0);
    e = m;
    CHECK(EncodeFacts(e, buf, 8 + 2 * 12) == 0);
    // the largest message fits the link's 1024 with the SVM1 header
    FactsMsg big;
    big.from = 2, big.n = static_cast<std::uint8_t>(kMaxFacts);
    for (std::size_t i = 0; i < kMaxFacts; ++i) big.e[i].host_id = static_cast<std::uint32_t>(0x20000 + i);
    CHECK(EncodeFacts(big, buf, sizeof buf) == FactsSize(kMaxFacts) && 8 + FactsSize(kMaxFacts) <= 1024);
    CHECK(DecodeFacts(buf, FactsSize(kMaxFacts), d) && d.n == kMaxFacts && d.from == 2);
    big.n = static_cast<std::uint8_t>(kMaxFacts + 1);
    CHECK(EncodeFacts(big, buf, sizeof buf) == 0);
    // an empty message is valid (no trucks)
    FactsMsg none;
    none.from = 2;
    CHECK(EncodeFacts(none, buf, sizeof buf) == 8 && DecodeFacts(buf, 8, d) && d.n == 0);
    // change detection
    Fact a = m.e[0], b = a;
    b.seat = 1, b.flags = 0;
    CHECK(!FactChanged(a, b));
    b.driver_id = kNoActor, b.driver = Driver::none;
    CHECK(FactChanged(a, b));
}

// round 28 T1 (vehicle_mule_v3): the STV3 hello, the gate, the host's MULE-driver test and its pacing
void TestV3() {
    // the hello round-trips; strict decode
    V3Hello h;
    h.features = kV3FeatHostTrucks, h.role = 1;
    std::uint8_t b[16] = {};
    CHECK(EncodeV3Hello(h, b, sizeof(b)) == kV3HelloSize);
    CHECK(std::memcmp(b, "STV3", 4) == 0 && b[4] == kV3Version && b[5] == 1 && b[6] == 1 && b[7] == 0);
    V3Hello back;
    CHECK(DecodeV3Hello(b, kV3HelloSize, back) && back.version == 1 && back.features == kV3FeatHostTrucks && back.role == 1);
    CHECK(!DecodeV3Hello(b, kV3HelloSize - 1, back) && !DecodeV3Hello(b, kV3HelloSize + 1, back) && !DecodeV3Hello(nullptr, kV3HelloSize, back));
    CHECK(EncodeV3Hello(h, b, kV3HelloSize - 1) == 0);
    V3Hello bad = h;
    bad.role = 3;
    CHECK(EncodeV3Hello(bad, b, sizeof(b)) == 0);
    bad = h, bad.version = 0;
    CHECK(EncodeV3Hello(bad, b, sizeof(b)) == 0);
    EncodeV3Hello(h, b, sizeof(b));
    b[7] = 1;
    CHECK(!DecodeV3Hello(b, kV3HelloSize, back));  // the reserved byte
    b[7] = 0, b[6] = 0;
    CHECK(!DecodeV3Hello(b, kV3HelloSize, back));  // no role
    b[6] = 2, b[4] = 0;
    CHECK(!DecodeV3Hello(b, kV3HelloSize, back));  // version 0
    b[4] = 2, b[5] = 0xFF;
    CHECK(DecodeV3Hello(b, kV3HelloSize, back) && back.version == 2 && back.features == kV3AllFeatures && back.role == 2);  // unknown bits dropped
    b[0] = 'X';
    CHECK(!DecodeV3Hello(b, kV3HelloSize, back));
    static_assert(kV3HelloType != kFactsType && kV3HelloType >= 16 && kV3HelloType <= 63, "STV3 hello type");
    // the gate: first failing condition wins
    V3GateIn g;
    CHECK(V3Gate(g) == V3State::key_off);
    g.key_on = true;
    CHECK(V3Gate(g) == V3State::drive_off);
    g.drive_on = true;
    CHECK(V3Gate(g) == V3State::drive_off);  // no role
    g.role = 2;
    CHECK(V3Gate(g) == V3State::no_hello);
    g.have_hello = true;
    CHECK(V3Gate(g) == V3State::old_session);
    g.same_session = true;
    CHECK(V3Gate(g) == V3State::stale_hello);  // the default age
    g.hello_age_ms = kV3HelloFreshMs + 1;
    CHECK(V3Gate(g) == V3State::stale_hello);
    g.hello_age_ms = 1500;
    CHECK(V3Gate(g) == V3State::no_feature);
    g.features = kV3FeatHostTrucks;
    CHECK(V3Gate(g) == V3State::same_role);  // peer role 0
    g.peer_role = 2;
    CHECK(V3Gate(g) == V3State::same_role);
    g.peer_role = 1;
    CHECK(V3Gate(g) == V3State::live);
    g.role = 1, g.peer_role = 2;
    CHECK(V3Gate(g) == V3State::live);
    g.key_on = false;
    CHECK(V3Gate(g) == V3State::key_off);
    for (int s = 0; s <= 7; ++s) CHECK(std::strcmp(V3StateName(static_cast<V3State>(s)), "?") != 0);
    // the host's MULE-driver test (run 20261127-002653: 0x2000F, driver 0x0202E, [+0x528] the MULE's ctl+0x30)
    NpcDriveIn n;
    CHECK(NpcDrive(n) == NpcWhy::no_seat);
    n.seat_read = true, n.has = 1, n.driver = 0x0202E;
    n.input = n.input_game = n.we_own = true;
    n.partner_m = 40;
    CHECK(NpcDrive(n) == NpcWhy::drives);
    NpcDriveIn x = n;
    x.has = 0;
    CHECK(NpcDrive(x) == NpcWhy::no_seat);
    x = n, x.driver = kNoActor;
    CHECK(NpcDrive(x) == NpcWhy::no_seat);  // our follower's own driver id
    x = n, x.driver = kSam;
    CHECK(NpcDrive(x) == NpcWhy::not_mule);
    x = n, x.driver = kOther;
    CHECK(NpcDrive(x) == NpcWhy::not_mule);
    x = n, x.input = false;
    CHECK(NpcDrive(x) == NpcWhy::no_input);
    x = n, x.input_game = false;
    CHECK(NpcDrive(x) == NpcWhy::not_game_input);
    x = n, x.follower_here = true;
    CHECK(NpcDrive(x) == NpcWhy::our_follower);
    x = n, x.sam_drives = true;
    CHECK(NpcDrive(x) == NpcWhy::our_sam);
    x = n, x.we_own = false;
    CHECK(NpcDrive(x) == NpcWhy::not_ours);
    x = n, x.partner_m = -1;
    CHECK(NpcDrive(x) == NpcWhy::partner_far);
    x = n, x.partner_m = kV3SendRangeM + 0.5;
    CHECK(NpcDrive(x) == NpcWhy::partner_far);
    x = n, x.partner_m = kV3SendRangeM;
    CHECK(NpcDrive(x) == NpcWhy::drives);
    for (int w = 0; w <= 8; ++w) CHECK(std::strcmp(NpcWhyName(static_cast<NpcWhy>(w)), "?") != 0);
    // pacing: every tick near, 10 Hz far
    CHECK(NpcSendDue(50, 0) && NpcSendDue(kV3NearM, 10));
    CHECK(!NpcSendDue(150, 50) && NpcSendDue(150, kV3FarGapMs) && !NpcSendDue(-1, 10) && NpcSendDue(-1, 200));
}

void TestKeys() {
    cfgval::TruckKeys k;
    CHECK(k.log && k.hold);
    CHECK(cfgval::SetTruckKey(k, "vehicle_mule_log", false) && !k.log && k.hold);
    CHECK(cfgval::SetTruckKey(k, "vehicle_mule_hold", false) && !k.hold);
    CHECK(!cfgval::SetTruckKey(k, "vehicle_mule", true) && !k.log && !k.hold);
}
}  // namespace

int main() {
    TestActorTypes();
    TestClassify();
    TestHold();
    TestPlayerLast();
    TestLatch();
    TestCodec();
    TestV3();
    TestKeys();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
