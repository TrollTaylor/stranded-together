// Round 28 P2 (native/mod/truck_v3_rules.hpp): the truck controller probe's identity verdict, seat math, get-off callers, the
// watch's change test and the action's ops.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "truck_v3_rules.hpp"

using namespace st;
using namespace st::truckv3;

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

constexpr std::uint32_t kMule = 0x2000u | 0x2E;  // type 1
constexpr std::uint32_t kTruck = 0x2000Fu;

// A MULE driving the truck, everything agreeing.
CtlRead Good() {
    CtlRead r;
    r.truck_id = kTruck, r.truck_entity = 0x5000, r.has_driver = 1, r.driver_id = kMule;
    r.actor = 0x9000, r.ctl30 = 0x9000 + 0x3D0 + 0x30;
    r.input = r.ctl30, r.input_vt = 0x1234, r.want_vt = 0x1234;
    r.actor_found = r.actor_is_mule = r.ride_ok = true;
    r.ctl_state = 3, r.ctl_driver = kMule, r.ctl_vehicle = 0x5000, r.drive_comp = 0x7000;
    r.seat = 0, r.ride_vehicle_id = kTruck, r.ride_vehicle = 0x5000, r.ride_bits = 1;
    return r;
}

void TestIdentity() {
    CtlRead r = Good();
    Verdict v = IdentityOf(r);
    CHECK(v.id == Identity::same && v.mis == 0 && v.Ok());
    CHECK(std::strcmp(IdentityName(v.id), "SAME") == 0);
    r.ctl_state = 2;
    CHECK(IdentityOf(r).Ok());  // attached, the engine off
    r.input = 0x8888;
    v = IdentityOf(r);
    CHECK(v.id == Identity::differs && !v.Ok());
    r = Good(), r.input = 0;
    v = IdentityOf(r);
    CHECK(v.id == Identity::no_input && !(v.mis & kMisInputVt));
    r = Good(), r.actor_found = false;
    CHECK(IdentityOf(r).id == Identity::no_actor);
    r = Good(), r.actor_is_mule = false;
    CHECK(IdentityOf(r).id == Identity::not_mule);
    r = Good(), r.ride_ok = false;
    CHECK(IdentityOf(r).id == Identity::unchecked);
    // each secondary fact
    r = Good(), r.input_vt = 1;
    CHECK(IdentityOf(r).mis == kMisInputVt);
    r = Good(), r.ctl_driver = 0x2031;
    CHECK(IdentityOf(r).mis == kMisCtlDriver);
    r = Good(), r.ctl_vehicle = 0x6000;
    CHECK(IdentityOf(r).mis == kMisCtlVehicle);
    r = Good(), r.ctl_state = 1;
    CHECK(IdentityOf(r).mis == kMisCtlState);
    r = Good(), r.ride_vehicle_id = 0x20010;
    CHECK(IdentityOf(r).mis == kMisRideId);
    r = Good(), r.ride_vehicle = 0;
    CHECK(IdentityOf(r).mis == kMisRideHandle);
    r = Good(), r.ride_bits = 0;
    CHECK(IdentityOf(r).mis == kMisNotRiding);
    r = Good(), r.seat = 1;
    CHECK(IdentityOf(r).mis == kMisSeat);
    r = Good(), r.drive_comp = 0;
    CHECK(IdentityOf(r).mis == kMisNoDriveComp);
    r = Good(), r.has_driver = 0;
    CHECK(IdentityOf(r).mis == kMisHasDriver);
    // the text
    char t[160];
    MismatchText(0, t, sizeof(t));
    CHECK(t[0] == 0);
    MismatchText(kMisCtlDriver | kMisSeat, t, sizeof(t));
    CHECK(std::strcmp(t, "ctl-driver seat") == 0);
    char small[6];
    MismatchText(kMisInputVt | kMisCtlDriver, small, sizeof(small));
    CHECK(std::strlen(small) == 5 && std::strncmp(small, "input", 5) == 0);
    MismatchText(kMisInputVt, nullptr, 0);  // no crash
}

void TestTally() {
    Tally t;
    t.Add(IdentityOf(Good()));
    CtlRead r = Good();
    r.input = 0x8888;
    t.Add(IdentityOf(r));
    r = Good(), r.drive_comp = 0;
    t.Add(IdentityOf(r));
    r = Good(), r.actor_found = false;
    t.Add(IdentityOf(r));
    CHECK(t.checked == 4 && t.same == 2 && t.differs == 1 && t.other == 1 && t.agree == 1 && t.no_drive_comp == 1);
}

Xf Yawed(double x, double y, double z, double deg) {
    const double a = deg / 57.29577951308232, c = std::cos(a), s = std::sin(a);
    Xf v;
    v.pos[0] = x, v.pos[1] = y, v.pos[2] = z;
    // rows: 0 right, 1 forward, 2 up (z up), turned by `deg` about z
    v.rot[0][0] = static_cast<float>(c), v.rot[0][1] = static_cast<float>(-s), v.rot[0][2] = 0;
    v.rot[1][0] = static_cast<float>(s), v.rot[1][1] = static_cast<float>(c), v.rot[1][2] = 0;
    v.rot[2][0] = 0, v.rot[2][1] = 0, v.rot[2][2] = 1;
    return v;
}
bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

void TestSeat() {
    // identity frame: local == world - pos
    const Xf v0 = Yawed(100, 200, 10, 0);
    Xf w = Yawed(101.5, 199, 11.2, 0);
    double lp[3], lr[3][3];
    LocalOf(v0, w, lp, lr);
    CHECK(Near(lp[0], 1.5) && Near(lp[1], -1) && Near(lp[2], 1.2));
    CHECK(Near(lr[0][0], 1) && Near(lr[1][1], 1) && Near(lr[2][2], 1) && Near(lr[0][1], 0));
    CHECK(Near(LocalYawDeg(lr), 0));
    // a turned truck: the round trip
    const Xf v = Yawed(1666.75, 990.41, 174.93, 37);
    const double seat[3] = {0.55, 1.2, 0.9};
    const Xf mrot = Yawed(0, 0, 0, 37 + 90);  // the MULE faces the truck's right
    double srot[3][3];
    {
        double dummy[3];
        LocalOf(v, mrot, dummy, srot);
    }
    CHECK(Near(LocalYawDeg(srot), 90, 1e-3));
    const Xf world = SeatWorld(v, seat, srot);
    LocalOf(v, world, lp, lr);
    CHECK(Near(lp[0], seat[0]) && Near(lp[1], seat[1]) && Near(lp[2], seat[2]));
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) CHECK(Near(lr[i][k], srot[i][k], 1e-5));
    // the world position by hand: pos + sum local[j] * row j
    for (int i = 0; i < 3; ++i)
        CHECK(Near(world.pos[i], v.pos[i] + seat[0] * v.rot[0][i] + seat[1] * v.rot[1][i] + seat[2] * v.rot[2][i]));
    // sanity
    CHECK(XfSane(v));
    Xf bad = v;
    bad.pos[0] = NAN;
    CHECK(!XfSane(bad));
    bad = v, bad.rot[1][1] = 3.f;
    CHECK(!XfSane(bad));
    CHECK(!XfSane(Xf{}));  // a zero rotation: unread
}

void TestGetOff() {
    const char* n = nullptr;
    CHECK(GetOffSite(0x142B3AB63 + 5, &n) == 0x142B3AB63 && std::strstr(n, "abort") != nullptr);
    CHECK(GetOffSite(0x142AF563B + 5, &n) == 0x142AF563B && std::strcmp(n, "0x142AF5620") == 0);
    CHECK(GetOffSite(0x142B3AB63, &n) == 0 && std::strcmp(n, "unknown") == 0);  // the call itself, not its return
    CHECK(GetOffSite(0, nullptr) == 0);
    static_assert(sizeof(kGetOffCallers) / sizeof(kGetOffCallers[0]) == 6, "six get-off callers");
    CHECK(ToVa(0x7FF600001000ull, 0x7FF600000000ull, 0x8000000) == 0x140001000ull);
    CHECK(ToVa(0x7FF5FFFFFFFFull, 0x7FF600000000ull, 0x8000000) == 0);
    CHECK(ToVa(0x7FF608000000ull, 0x7FF600000000ull, 0x8000000) == 0);
    CHECK(ToVa(5, 0, 100) == 0);
}

void TestWatch() {
    Ride a;
    a.mule = kMule, a.truck = kTruck, a.seat = 0, a.ctl_state = 2, a.riding = true;
    Ride off = a;
    off.riding = false, off.truck = kNoActor, off.seat = -1;
    CHECK(RideChanged(off, true, a, true) == RideEvent::board);
    CHECK(RideChanged(Ride{}, false, a, true) == RideEvent::board);  // first sample, already riding
    CHECK(RideChanged(a, true, off, true) == RideEvent::leave);
    CHECK(RideChanged(a, true, a, false) == RideEvent::leave);
    CHECK(RideChanged(a, true, a, true) == RideEvent::none);
    CHECK(RideChanged(off, true, off, true) == RideEvent::none);
    Ride b = a;
    b.ctl_state = 3;
    CHECK(RideChanged(a, true, b, true) == RideEvent::state);
    b = a, b.seat = 1;
    CHECK(RideChanged(a, true, b, true) == RideEvent::seat);
    Ride p = b;  // a passenger: its controller state is not logged
    p.ctl_state = 0;
    CHECK(RideChanged(b, true, p, true) == RideEvent::none);
    b = a, b.truck = 0x20010;
    CHECK(RideChanged(a, true, b, true) == RideEvent::board);
    CHECK(std::strcmp(RideEventName(RideEvent::board), "board") == 0 && std::strcmp(RideEventName(RideEvent::state), "ctl") == 0);
}

void TestDriverWatch() {
    CHECK(DriverChanged(kNoActor, false, kNoActor) == DrvEvent::none);
    CHECK(DriverChanged(0, false, 0x0202E) == DrvEvent::set);  // the first sample with a driver
    CHECK(DriverChanged(kNoActor, true, 0x0202E) == DrvEvent::set);
    CHECK(DriverChanged(0x0202E, true, 0x0202E) == DrvEvent::none);
    CHECK(DriverChanged(0x0202E, true, kNoActor) == DrvEvent::cleared);
    CHECK(DriverChanged(0x0202E, true, 0xFFFFFFFFu) == DrvEvent::cleared);
    CHECK(DriverChanged(0x0202E, true, 0x0202D) == DrvEvent::changed);
    CHECK(DriverChanged(0x0202E, false, kNoActor) == DrvEvent::none);  // no previous sample: nothing to clear
    CHECK(std::strcmp(DrvEventName(DrvEvent::set), "set") == 0 && std::strcmp(DrvEventName(DrvEvent::cleared), "cleared") == 0);
    CHECK(!SeatedOf(false, false) && SeatedOf(true, false) && SeatedOf(false, true));
    CHECK(SeatedTruck(true, 0x2000F, false, kNoActor) == 0x2000F);
    CHECK(SeatedTruck(true, kNoActor, true, 0x20010) == 0x20010);  // the bit without a vehicle id: the chain's
    CHECK(SeatedTruck(false, kNoActor, true, 0x2000F) == 0x2000F);
    CHECK(SeatedTruck(false, kNoActor, false, kNoActor) == kNoActor);
}

void TestOps() {
    CHECK(OpOf(0) == Op::off && OpOf(1) == Op::on && OpOf(8) == Op::seats && OpOf(9) == Op::ident);
    CHECK(OpOf(3) == Op::v3 && OpOf(3.5) == Op::bad);  // round 28 T1
    CHECK(OpOf(2) == Op::bad && OpOf(-1) == Op::bad && OpOf(1.5) == Op::bad && OpOf(NAN) == Op::bad && OpOf(INFINITY) == Op::bad);
    CHECK(PeriodOf(0) == 1.0 && PeriodOf(-3) == 1.0 && PeriodOf(NAN) == 1.0 && PeriodOf(0.05) == 0.2 && PeriodOf(60) == 10.0 && PeriodOf(2.5) == 2.5);
}
}  // namespace

int main() {
    TestIdentity();
    TestTally();
    TestSeat();
    TestGetOff();
    TestWatch();
    TestDriverWatch();
    TestOps();
    std::printf("truck_v3_rules: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
