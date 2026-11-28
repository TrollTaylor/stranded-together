// Round 28 P2 (native/mod/truck_v3_rules.hpp): the truck controller probe's identity verdict, seat math, get-off callers, the
// watch's change test and the action's ops; round 29 T2: the STO1 codec (with a fuzz), the occupant entries, the seat placement,
// the show / hide decision and the shove rule.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
    char tiny[6];  // (not `small`: an RPC header's macro)
    MismatchText(kMisInputVt | kMisCtlDriver, tiny, sizeof(tiny));
    CHECK(std::strlen(tiny) == 5 && std::strncmp(tiny, "input", 5) == 0);
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
    CHECK(OpOf(4) == Op::seat && OpOf(4.5) == Op::bad);  // round 29 T2
    CHECK(OpOf(10) == Op::conv && OpOf(10.5) == Op::bad && OpOf(11) == Op::bad);  // round 30 stage 0 (P2)
    CHECK(OpOf(2) == Op::bad && OpOf(-1) == Op::bad && OpOf(1.5) == Op::bad && OpOf(NAN) == Op::bad && OpOf(INFINITY) == Op::bad);
    CHECK(PeriodOf(0) == 1.0 && PeriodOf(-3) == 1.0 && PeriodOf(NAN) == 1.0 && PeriodOf(0.05) == 0.2 && PeriodOf(60) == 10.0 && PeriodOf(2.5) == 2.5);
}

// ---------------------------------------------------------------- round 29 T2 (vehicle_mule_seat)
OccFrame TwoSeated() {
    OccFrame f;
    f.seq = 7, f.truck = kTruck, f.n = 2;
    f.e[0].mule = kMule, f.e[0].seat = 0, f.e[0].src = kOccSrcChain;
    f.e[0].lpos[0] = -0.437, f.e[0].lpos[1] = 0.810, f.e[0].lpos[2] = 0.157;
    const double h = std::sqrt(0.5);
    f.e[0].q[0] = 0, f.e[0].q[1] = 0, f.e[0].q[2] = h, f.e[0].q[3] = h;  // 90 degrees about row 2
    f.e[1].mule = kMule + 1, f.e[1].seat = 2, f.e[1].src = kOccSrcTable;
    f.e[1].lpos[0] = -1.299, f.e[1].lpos[1] = 0.127, f.e[1].lpos[2] = 0.778;
    return f;
}

void TestOccCodec() {
    static_assert(kOccType >= 16 && kOccType <= 63, "an SVM1 external type");
    static_assert(kOccType != trucks::kV3HelloType, "not the hello's type");
    static_assert(kOccMaxBody == 236 && kOccMaxBody + 8 <= 1024, "fits vwire::kMaxMessage with the header");
    static_assert((trucks::kV3AllFeatures & trucks::kV3FeatSeats) && (trucks::kV3FeatSeats & trucks::kV3FeatHostTrucks) == 0, "the feature bit");
    std::uint8_t b[kOccMaxBody + 16] = {};
    const OccFrame f = TwoSeated();
    const std::size_t n = EncodeOcc(f, b, sizeof(b));
    CHECK(n == kOccHeader + 2 * kOccEntry);
    CHECK(std::memcmp(b, "STO1", 4) == 0 && b[4] == 1 && b[5] == 2 && b[6] == 7 && b[7] == 0 && b[8] == 0x0F && b[10] == 0x02);
    OccFrame back;
    CHECK(DecodeOcc(b, n, back));
    CHECK(back.seq == 7 && back.truck == kTruck && back.n == 2 && back.e[0].mule == kMule && back.e[0].seat == 0 && back.e[0].src == kOccSrcChain);
    CHECK(back.e[1].mule == kMule + 1 && back.e[1].seat == 2 && back.e[1].src == kOccSrcTable);
    CHECK(Near(back.e[0].lpos[0], -0.437, 1e-6) && Near(back.e[0].lpos[1], 0.810, 1e-6) && Near(back.e[0].lpos[2], 0.157, 1e-6));
    CHECK(Near(back.e[0].q[2], std::sqrt(0.5), 1e-4) && Near(back.e[0].q[3], std::sqrt(0.5), 1e-4) && Near(back.e[1].q[3], 1.0, 1e-6));
    CHECK(!OccChanged(f, back));  // the quantization is far below the change test's 2 cm / 2 degrees
    // n = 0: the clear
    OccFrame clear;
    clear.truck = kTruck, clear.seq = 0xFFFF;
    CHECK(EncodeOcc(clear, b, sizeof(b)) == kOccHeader);
    CHECK(DecodeOcc(b, kOccHeader, back) && back.n == 0 && back.truck == kTruck && back.seq == 0xFFFF);
    // the encoder refuses what the decoder would
    OccFrame bad = f;
    bad.truck = 0;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.truck = kNoActor;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[1].mule = kMule;  // a duplicate
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[0].mule = kTruck;  // not a MULE id (actor type 16)
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[0].seat = 8;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[0].seat = kOccSeatUnknown;
    CHECK(EncodeOcc(bad, b, sizeof(b)) != 0);  // an unknown seat is allowed
    bad = f, bad.e[0].src = 3;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[0].lpos[1] = 5.5;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[0].lpos[2] = NAN;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.e[0].q[3] = 0, bad.e[0].q[2] = 0.5;  // not a unit quaternion
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    bad = f, bad.n = 9;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == 0);
    CHECK(EncodeOcc(f, b, n - 1) == 0 && EncodeOcc(f, nullptr, sizeof(b)) == 0);
    // a negative w goes out as the same rotation with w >= 0
    bad = f;
    for (double& c : bad.e[0].q) c = -c;
    CHECK(EncodeOcc(bad, b, sizeof(b)) == n && DecodeOcc(b, n, back) && back.e[0].q[3] > 0 && Near(back.e[0].q[2], std::sqrt(0.5), 1e-4));
    // strict decode: every byte rule
    EncodeOcc(f, b, sizeof(b));
    std::uint8_t c[sizeof(b)];
    auto mutate = [&](std::size_t at, std::uint8_t v) {
        std::memcpy(c, b, sizeof(c));
        c[at] = v;
        return DecodeOcc(c, n, back);
    };
    CHECK(!DecodeOcc(b, n - 1, back) && !DecodeOcc(b, n + 1, back) && !DecodeOcc(nullptr, n, back) && !DecodeOcc(b, 11, back));
    CHECK(!mutate(0, 'X'));                    // the magic
    CHECK(!mutate(4, 2) && !mutate(4, 0));     // the version
    CHECK(!mutate(5, 1) && !mutate(5, 3));     // n against the size
    CHECK(!mutate(5, 9));                      // n > 8
    std::memcpy(c, b, sizeof(c));
    c[8] = c[9] = c[10] = c[11] = 0;
    CHECK(!DecodeOcc(c, n, back));             // truck id 0
    std::memcpy(c, b, sizeof(c));
    c[8] = 0xFF, c[9] = 0xFF, c[10] = 0x1F, c[11] = 0;
    CHECK(!DecodeOcc(c, n, back));             // truck id kNoActor
    CHECK(!mutate(kOccHeader + 6, 1) && !mutate(kOccHeader + 7, 1));  // the pad
    CHECK(!mutate(kOccHeader + 4, 8));         // seat 8
    CHECK(mutate(kOccHeader + 4, 0xFF));       // seat unknown
    CHECK(!mutate(kOccHeader + 5, 0) && !mutate(kOccHeader + 5, 3));  // src
    CHECK(!mutate(kOccHeader + 2, 0x20));      // the MULE id out of range (0x20202E >= kNoActor)
    CHECK(!mutate(kOccHeader + 1, 0x40));      // the MULE id's type (0x402E: type 2, not a MULE)
    CHECK(!mutate(kOccHeader + 11, 0x7F));     // x = NaN (an f32 exponent of all ones)
    CHECK(!mutate(kOccHeader + 11, 0x41));     // x = +28 m (over 5 m)
    std::memcpy(c, b, sizeof(c));
    std::memcpy(c + kOccHeader + kOccEntry, c, 4);  // entry 1's id = entry 0's
    CHECK(!DecodeOcc(c, n, back));
    std::memcpy(c, b, sizeof(c));
    std::memset(c + kOccHeader + 20, 0, 8);    // a zero quaternion
    CHECK(!DecodeOcc(c, n, back));
    CHECK(OccSeqNewer(8, 7) && !OccSeqNewer(7, 7) && !OccSeqNewer(6, 7) && OccSeqNewer(0, 0xFFFF) && !OccSeqNewer(0xFFFF, 0));
}

// The bytes a decode may read: a case is copied to the very end of a readable page followed by an inaccessible one (Windows), so
// a read past its size faults the test; elsewhere an exactly sized heap copy.
struct Guarded {
#ifdef _WIN32
    std::uint8_t* page = nullptr;
    std::size_t page_size = 0;
    Guarded() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_size = si.dwPageSize;
        page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 2 * page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (page) VirtualProtect(page + page_size, page_size, PAGE_NOACCESS, &old);
    }
    ~Guarded() {
        if (page) VirtualFree(page, 0, MEM_RELEASE);
    }
    bool Decode(const std::uint8_t* b, std::size_t size, OccFrame& f) {
        if (!page) return false;
        std::uint8_t* at = page + page_size - size;
        if (size) std::memcpy(at, b, size);
        return DecodeOcc(at, size, f);
    }
#else
    bool Decode(const std::uint8_t* b, std::size_t size, OccFrame& f) {
        std::vector<std::uint8_t> v(b, b + size);
        return DecodeOcc(v.data(), size, f);
    }
#endif
};

// The codec under random input: never a read past the buffer (every case, and every prefix of it, decoded from the end of a page
// followed by an inaccessible one), and whatever decodes re-encodes to a frame that decodes to the same occupants.
void TestOccFuzz() {
    std::uint32_t s = 0x1234567u;
    auto rnd = [&]() { return s = s * 1664525u + 1013904223u; };
    std::uint8_t base[kOccMaxBody];
    const std::size_t n = EncodeOcc(TwoSeated(), base, sizeof(base));
    unsigned decoded = 0, decoded_random = 0, mutated = 0, prefixes = 0, bad_prefixes = 0;
    Guarded g;
#ifdef _WIN32
    CHECK(g.page != nullptr);
#endif
    for (int it = 0; it < 40000; ++it) {
        std::uint8_t b[kOccMaxBody];
        std::size_t size;
        const bool random = it % 4 == 0;
        if (random) {  // random bytes with the magic, a plausible size, a truck id in range and mostly MULE ids
            size = kOccHeader + kOccEntry * (rnd() % 9);
            for (std::size_t i = 0; i < size; ++i) b[i] = static_cast<std::uint8_t>(rnd() >> 24);
            std::memcpy(b, "STO1", 4);
            b[4] = 1, b[5] = static_cast<std::uint8_t>((size - kOccHeader) / kOccEntry);
            const std::uint32_t truck = 1 + rnd() % (kNoActor - 1);
            for (int i = 0; i < 4; ++i) b[8 + i] = static_cast<std::uint8_t>(truck >> (8 * i));
            for (unsigned e = 0; e < b[5]; ++e) {
                std::uint8_t* p = b + kOccHeader + kOccEntry * e;
                if (rnd() % 8 == 0) continue;  // a random id, as it came
                const std::uint32_t mule = 0x2000u | (rnd() % 0x2000u);  // type 1
                for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(mule >> (8 * i));
                p[4] = static_cast<std::uint8_t>(rnd() % 10), p[5] = static_cast<std::uint8_t>(1 + rnd() % 2);
                if (rnd() % 4) p[6] = p[7] = 0;
                for (int k = 0; k < 3; ++k) {  // a place within 6 m: mostly in range
                    const float v = static_cast<float>((static_cast<int>(rnd() % 12001) - 6000) / 1000.0);
                    std::memcpy(p + 8 + 4 * k, &v, 4);
                }
                if (rnd() % 3) {  // mostly a near-unit quaternion
                    const double q[4] = {(rnd() % 2001 - 1000.0) / 2000.0, (rnd() % 2001 - 1000.0) / 2000.0, (rnd() % 2001 - 1000.0) / 2000.0, 1.0};
                    const double qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                    for (int k = 0; k < 4; ++k) {
                        const auto v = static_cast<std::int16_t>(std::lround(q[k] / qn * 32767.0));
                        p[20 + 2 * k] = static_cast<std::uint8_t>(v), p[21 + 2 * k] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(v) >> 8);
                    }
                }
            }
        } else {  // a valid frame with 1..4 bytes changed, sometimes a size off by a little
            std::memcpy(b, base, n);
            size = n;
            const unsigned k = 1 + rnd() % 4;
            for (unsigned j = 0; j < k; ++j) b[rnd() % n] = static_cast<std::uint8_t>(rnd() >> 24);
            if (rnd() % 8 == 0) size = n - 1 - rnd() % 8;
            ++mutated;
        }
        OccFrame f;
        // a prefix is a frame only at the size its n gives (the exact size rule)
        for (std::size_t p = 0; p < size; ++p, ++prefixes)
            if (g.Decode(b, p, f) && p != kOccHeader + kOccEntry * b[5]) ++bad_prefixes;
        if (!g.Decode(b, size, f)) continue;
        ++decoded;
        if (random) ++decoded_random;
        CHECK(f.n <= kOccMaxPerTruck && OccTruckIdOk(f.truck));
        std::uint8_t r[kOccMaxBody];
        const std::size_t rn = EncodeOcc(f, r, sizeof(r));
        OccFrame h;
        CHECK(rn == size && g.Decode(r, rn, h) && !OccChanged(f, h) && h.seq == f.seq);
        for (unsigned i = 0; i < f.n; ++i) CHECK(OccEntryOk(f.e[i]));
    }
    // both kinds reached the strict entry rules (the random half decodes frames too, not only the header's rejections)
    CHECK(decoded > 100 && mutated > 1000 && decoded_random > 100 && prefixes > 100000 && bad_prefixes == 0);
    std::printf("occ fuzz: %u decoded (%u of the random half), %u prefixes read at a page's end\n", decoded, decoded_random, prefixes);
}

void TestOccEntry() {
    // the quaternion round trip on arbitrary rotations (yaw, then a pitch and a roll)
    for (double yaw = -170; yaw <= 180; yaw += 37.5)
        for (double pitch = -60; pitch <= 60; pitch += 30) {
            const double cy = std::cos(yaw / 57.29577951308232), sy = std::sin(yaw / 57.29577951308232);
            const double cp = std::cos(pitch / 57.29577951308232), sp = std::sin(pitch / 57.29577951308232);
            const double ry[3][3] = {{cy, sy, 0}, {-sy, cy, 0}, {0, 0, 1}};
            const double rp[3][3] = {{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
            double m[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) m[i][j] = rp[i][0] * ry[0][j] + rp[i][1] * ry[1][j] + rp[i][2] * ry[2][j];
            CHECK(RotOk(m));
            double q[4], back[3][3];
            MatToQuat(m, q);
            CHECK(q[3] >= 0 && Near(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.0, 1e-9));
            QuatToMat(q, back);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) CHECK(Near(back[i][j], m[i][j], 1e-9));
        }
    const double id[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const double mirror[3][3] = {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const double scaled[3][3] = {{1.2, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    CHECK(RotOk(id) && !RotOk(mirror) && !RotOk(scaled));
    // the seat table
    double p[3];
    CHECK(SeatTableLocal(0, p) && Near(p[0], -0.437, 1e-9) && Near(p[1], 0.810, 1e-9) && Near(p[2], 0.157, 1e-9));
    CHECK(SeatTableLocal(3, p) && Near(p[0], 1.299, 1e-9) && SeatTableLocal(4, p) && !SeatTableLocal(5, p) && !SeatTableLocal(kOccSeatUnknown, p));
    const double near1[3] = {1.2, 0.2, 0.7}, nowhere[3] = {0, -3, 0};
    CHECK(SeatNearest(near1) == 1 && SeatNearest(nowhere) == kOccSeatUnknown);
    // the entry: the chain's transform when sane, else the table
    const double lp[3] = {-0.43, 0.81, 0.16};
    OccEntry e;
    CHECK(OccEntryOf(kMule, 0, true, lp, id, e) && e.src == kOccSrcChain && e.seat == 0 && Near(e.lpos[0], -0.43, 1e-12) && Near(e.q[3], 1.0, 1e-12));
    CHECK(OccEntryOf(kMule, -1, true, lp, id, e) && e.seat == 0);  // no ride seat: the nearest table seat
    CHECK(OccEntryOf(kMule, 2, false, lp, id, e) && e.src == kOccSrcTable && e.seat == 2 && Near(e.lpos[0], -1.299, 1e-12));
    CHECK(OccEntryOf(kMule, 2, true, lp, mirror, e) && e.src == kOccSrcTable);  // a reflection: the table
    const double far_p[3] = {0, 4.5, 0};
    CHECK(OccEntryOf(kMule, 1, true, far_p, id, e) && e.src == kOccSrcTable && e.seat == 1);  // over 4 m: the table
    CHECK(!OccEntryOf(kMule, -1, false, lp, id, e));   // no transform, no seat: not sent
    CHECK(!OccEntryOf(kMule, 6, false, lp, id, e));    // a seat the table does not know
    CHECK(!OccEntryOf(kTruck, 0, true, lp, id, e));    // not a MULE id
    // the change test
    OccFrame a = TwoSeated(), b = a;
    CHECK(!OccChanged(a, b));
    b.e[0].lpos[0] += 0.015;
    CHECK(!OccChanged(a, b));
    b.e[0].lpos[0] += 0.01;
    CHECK(OccChanged(a, b));
    b = a, std::swap(b.e[0], b.e[1]);
    CHECK(!OccChanged(a, b));  // the order does not matter
    b = a, b.e[1].seat = 1;
    CHECK(OccChanged(a, b));
    b = a, b.n = 1;
    CHECK(OccChanged(a, b));
    b = a, b.e[1].mule = kMule + 5;
    CHECK(OccChanged(a, b));
    b = a;  // 3 degrees about row 2
    const double t = 1.5 / 57.29577951308232;
    b.e[1].q[2] = std::sin(t), b.e[1].q[3] = std::cos(t);
    CHECK(OccChanged(a, b));
    b.e[1].q[2] = std::sin(0.5 * t), b.e[1].q[3] = std::cos(0.5 * t);  // 1.5 degrees
    CHECK(!OccChanged(a, b));
}

void TestSeatPlace() {
    // the truck yawed 90 degrees at (100, 200, 10), the driver's seat with the identity rotation: world = truck * local
    Xf truck = Yawed(100, 200, 10, 90);
    OccEntry e;
    SeatTableLocal(0, e.lpos);
    const double no_lead[3] = {0, 0, 0}, lead[3] = {0.5, -0.25, 0.0};
    Xf led, exact;
    SeatPlaceOf(truck, no_lead, e, led, exact);
    double lp[3], lr[3][3];
    LocalOf(truck, exact, lp, lr);  // back: the same local transform
    CHECK(Near(lp[0], -0.437, 1e-5) && Near(lp[1], 0.810, 1e-5) && Near(lp[2], 0.157, 1e-5));
    CHECK(Near(lr[0][0], 1, 1e-5) && Near(lr[1][1], 1, 1e-5) && Near(lr[2][2], 1, 1e-5));
    CHECK(PosDist(led.pos, exact.pos) < 1e-9);
    CHECK(Near(PosDist(exact.pos, truck.pos), std::sqrt(0.437 * 0.437 + 0.810 * 0.810 + 0.157 * 0.157), 1e-6));
    SeatPlaceOf(truck, lead, e, led, exact);
    CHECK(Near(led.pos[0] - exact.pos[0], 0.5, 1e-9) && Near(led.pos[1] - exact.pos[1], -0.25, 1e-9) && Near(led.pos[2] - exact.pos[2], 0, 1e-9));
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) CHECK(Near(led.rot[i][j], exact.rot[i][j], 1e-9));  // the lead moves, never turns
    // a local rotation: the seated MULE turned 90 degrees in the truck: world rows = local rows * truck rows
    const double h = std::sqrt(0.5);
    e.q[0] = 0, e.q[1] = 0, e.q[2] = h, e.q[3] = h;
    SeatPlaceOf(truck, no_lead, e, led, exact);
    LocalOf(truck, exact, lp, lr);
    double want[3][3];
    QuatToMat(e.q, want);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) CHECK(Near(lr[i][j], want[i][j], 1e-5));
    CHECK(Near(std::fabs(LocalYawDeg(lr)), 90, 1e-3));
    // the meter's rule: the previous tick's placement, no follower correction since 150 ms before it, the seat's move explained
    SeatSample m;
    m.placed_prev_tick = true, m.placed_ms = 1000, m.now_ms = 1033, m.corr_ms = 500, m.speed_mps = 6.0, m.seat_moved_m = 0.2;
    CHECK(SeatErrCounts(m));
    SeatSample x = m;
    x.placed_prev_tick = false;
    CHECK(!SeatErrCounts(x));  // placed some earlier tick (a skipped GuestTick)
    x = m, x.first_shown_tick = true;
    CHECK(!SeatErrCounts(x));
    x = m, x.corr_ms = 1010;
    CHECK(!SeatErrCounts(x));  // her follower snapped the truck after the placement (a parked 1.1 m teleport)
    x = m, x.corr_ms = 900;
    CHECK(!SeatErrCounts(x));  // ... or just before it (the teleport lands a frame later)
    x = m, x.corr_ms = 849;
    CHECK(SeatErrCounts(x));
    x = m, x.seat_moved_m = 6.0 * 0.033 + 0.29;
    CHECK(SeatErrCounts(x));
    x = m, x.seat_moved_m = 6.0 * 0.033 + 0.31;
    CHECK(!SeatErrCounts(x));  // a jump the truck's speed does not explain (a correction the stamp missed)
    x = m, x.seat_moved_m = NAN;
    CHECK(!SeatErrCounts(x));
    x = m, x.speed_mps = 0, x.seat_moved_m = 0.25;
    CHECK(SeatErrCounts(x));
    // out of her truck: beside it at the seat's side, clear of its box; a seat is inside the box
    double out[3];
    const double right[3] = {1.299, 0.127, 0.778}, left[3] = {-0.437, 0.810, 0.157}, rear[3] = {1.299, -3.5, 0.778};
    SeatOutLocal(right, out);
    CHECK(out[0] == kSeatOutSideM && Near(out[1], 0.127, 1e-12) && out[2] == 0);
    SeatOutLocal(left, out);
    CHECK(out[0] == -kSeatOutSideM);
    SeatOutLocal(rear, out);
    CHECK(out[1] == -2.0);
    const double none[3] = {NAN, NAN, NAN};
    SeatOutLocal(none, out);
    CHECK(out[0] == -kSeatOutSideM && out[1] == 0);
    const double id_rot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const SeatPos& sp : kSeatTable) {
        const Xf seat = SeatWorld(truck, sp.p, id_rot);
        CHECK(!SeatClearOf(truck, seat.pos));
        SeatOutLocal(sp.p, out);
        const Xf beside = SeatWorld(truck, out, id_rot);
        CHECK(SeatClearOf(truck, beside.pos));
    }
    const double nan_p[3] = {NAN, 0, 0};
    CHECK(!SeatClearOf(truck, nan_p));
    const double ahead[3] = {truck.pos[0] + 5 * truck.rot[1][0], truck.pos[1] + 5 * truck.rot[1][1], truck.pos[2]};
    CHECK(SeatClearOf(truck, ahead));  // 5 m ahead of it
}

void TestSeatDecide() {
    SeatIn s;
    CHECK(SeatDecide(s) == SeatWhy::off);
    s.on = true;
    CHECK(SeatDecide(s) == SeatWhy::no_record);
    s.record = true;
    CHECK(SeatDecide(s) == SeatWhy::not_followed);
    s.followed = true;
    CHECK(SeatDecide(s) == SeatWhy::no_pose);
    s.host_pose = true;
    CHECK(SeatDecide(s) == SeatWhy::no_capsule);  // round 29 T2 review: never shown with its collision in the world
    s.capsule = true;
    CHECK(SeatDecide(s) == SeatWhy::no_slot);
    s.slot = true;
    CHECK(SeatDecide(s) == SeatWhy::shown);
    s.capsule = false;
    CHECK(SeatDecide(s) == SeatWhy::no_capsule);
    s.capsule = true;
    s.record = false;  // the first failing condition wins
    CHECK(SeatDecide(s) == SeatWhy::no_record);
    s.record = true, s.on = false;
    CHECK(SeatDecide(s) == SeatWhy::off);
    CHECK(std::strcmp(SeatWhyName(SeatWhy::shown), "shown") == 0 && std::strcmp(SeatWhyName(SeatWhy::no_slot), "no pose slot") == 0 &&
          std::strcmp(SeatWhyName(SeatWhy::no_record), "no occupant record") == 0 &&
          std::strcmp(SeatWhyName(SeatWhy::no_capsule), "no capsule out of the world") == 0);
    for (unsigned w = 0; w < kSeatWhyCount; ++w) CHECK(std::strcmp(SeatWhyName(static_cast<SeatWhy>(w)), "?") != 0);
    // the shove rule
    CHECK(ShoveSpike(0.9, 0.1) && !ShoveSpike(0.7, 0.0) && !ShoveSpike(1.2, 0.9) && !ShoveSpike(NAN, 0) && !ShoveSpike(1.0, NAN));
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
    TestOccCodec();
    TestOccFuzz();
    TestOccEntry();
    TestSeatPlace();
    TestSeatDecide();
    std::printf("truck_v3_rules: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
