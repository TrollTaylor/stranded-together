// STQ1 (native/mod/ride_wire.*), riding together's pure part: the message round trip and every decoder rejection, the seats,
// boarding, the driver's answer, the forced exits, the hold / flip / crash detectors and the exit placement. No game, no link.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ride_wire.hpp"
#include "vehicle_wire.hpp"

using namespace st::rwire;

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

constexpr std::uint32_t kTruck = 0x020005, kRoadster = 0x040003, kTrike = 0x01E001;

Msg Req() {
    Msg m;
    m.type = MsgType::request;
    m.seat = Seat::passenger;
    m.reason = kReqHold;
    m.from = 1;
    m.ride_id = 0x1234;
    m.sender_ms = 987654;
    m.vehicle_id = kTruck;
    m.kind = 2;
    m.world = 3;
    return m;
}

bool Same(const Msg& a, const Msg& b) {
    return a.type == b.type && a.seat == b.seat && a.reason == b.reason && a.from == b.from && a.ride_id == b.ride_id && a.sender_ms == b.sender_ms &&
           a.vehicle_id == b.vehicle_id && a.kind == b.kind && a.world == b.world;
}

void TestMessages() {
    std::uint8_t buf[64];
    // every type round trips
    for (std::uint8_t t = 1; t <= 4; ++t) {
        Msg m = Req();
        m.type = static_cast<MsgType>(t);
        m.reason = t == 3 ? static_cast<std::uint8_t>(Deny::not_near) : t == 4 ? static_cast<std::uint8_t>(End::crash) : 0;
        const std::size_t n = EncodeMsg(m, buf, sizeof(buf));
        CHECK(n == kMessageSize);
        CHECK(n == 36);
        Msg o;
        CHECK(DecodeMsg(buf, n, o));
        CHECK(Same(m, o));
        std::uint8_t type = 0;
        CHECK(st::vwire::PeekType(buf, n, &type) && type == kSvmType);
    }
    // the trike carries a pillion, the roadster a passenger
    {
        Msg m = Req();
        m.vehicle_id = kTrike, m.kind = 3, m.seat = Seat::pillion;
        const std::size_t n = EncodeMsg(m, buf, sizeof(buf));
        Msg o;
        CHECK(n && DecodeMsg(buf, n, o) && o.seat == Seat::pillion && o.kind == 3);
        m.seat = Seat::passenger;
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m.vehicle_id = kRoadster, m.kind = 1;
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == kMessageSize);
    }
    // the encoder refuses what the decoder would
    {
        Msg m = Req();
        m.ride_id = 0;
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.kind = 3;  // a truck id with the trike's kind
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.from = 2;
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.reason = 4;  // REQUEST reasons 0..3
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.reason = kReqResume;  // v0.21: the seat back after a save
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == kMessageSize);
        m = Req(), m.reason = kReqNative;  // the game's own Use Vehicle
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == kMessageSize);
        m = Req(), m.type = MsgType::deny, m.reason = static_cast<std::uint8_t>(Deny::count);
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.type = MsgType::end, m.reason = static_cast<std::uint8_t>(End::count);
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.type = MsgType::accept, m.reason = 1;
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req(), m.type = static_cast<MsgType>(5);
        CHECK(EncodeMsg(m, buf, sizeof(buf)) == 0);
        m = Req();
        CHECK(EncodeMsg(m, buf, kMessageSize - 1) == 0);
    }
    // the decoder: every byte that matters
    {
        Msg m = Req();
        const std::size_t n = EncodeMsg(m, buf, sizeof(buf));
        Msg o;
        CHECK(!DecodeMsg(buf, n - 1, o));
        std::vector<std::uint8_t> longer(buf, buf + n);
        longer.push_back(0);
        CHECK(!DecodeMsg(longer.data(), longer.size(), o));
        CHECK(!DecodeMsg(nullptr, n, o));
        const auto flip = [&](std::size_t at, std::uint8_t v) {
            std::uint8_t b[64];
            std::memcpy(b, buf, n);
            b[at] = v;
            Msg x;
            return DecodeMsg(b, n, x);
        };
        CHECK(!flip(0, 'X'));     // SVM1 magic
        CHECK(!flip(4, 47));      // another SVM1 type
        CHECK(!flip(5, 2));       // SVM1 version
        CHECK(!flip(6, 1));       // SVM1 reserved
        CHECK(!flip(8, 'X'));     // STQ1 magic
        CHECK(!flip(11, '2'));
        CHECK(!flip(12, 0));      // type 0
        CHECK(!flip(12, 5));      // type 5
        CHECK(!flip(13, 2));      // pillion on a truck
        CHECK(!flip(13, 0));      // no seat
        CHECK(!flip(14, 9));      // request reason
        CHECK(!flip(14, 4));      // request reason 4 (0 hold, 1 test, 2 native, 3 resume)
        CHECK(!flip(15, 2));      // from
        CHECK(!flip(28, 3));      // kind 3 with a truck id
        CHECK(!flip(28, 0));
        CHECK(!flip(29, 1));      // reserved
        CHECK(!flip(30, 1));
        CHECK(!flip(31, 1));
        std::uint8_t b[64];
        std::memcpy(b, buf, n);
        std::memset(b + 16, 0, 4);  // ride id 0
        CHECK(!DecodeMsg(b, n, o));
        std::memcpy(b, buf, n);
        b[24] = 0x00, b[25] = 0x00, b[26] = 0x00, b[27] = 0x00;  // vehicle id 0: no kind
        CHECK(!DecodeMsg(b, n, o));
        // the sender time and the world are free values
        std::memcpy(b, buf, n);
        b[20] = 0xFF, b[32] = 0xFF;
        CHECK(DecodeMsg(b, n, o));
    }
    CHECK(std::strcmp(MsgTypeName(1), "REQUEST") == 0 && std::strcmp(MsgTypeName(9), "?") == 0);
    CHECK(std::strcmp(EndName(static_cast<std::uint8_t>(End::crash)), "crash") == 0);
    CHECK(std::strcmp(DenyName(200), "?") == 0);
    CHECK(std::strcmp(BlockName(Block::too_far), "too far") == 0);
}

void TestSeats() {
    float p[3], d[3], door[3];
    for (std::uint8_t k = 1; k <= 3; ++k) {
        CHECK(PassengerSeat(k, p));
        CHECK(DriverSeat(k, d));
        CHECK(DoorPoint(k, door));
        CHECK(SeatOf(k) != Seat::none);
        // every seat inside the vehicle's footprint, the door outside it on the right
        float half[3];
        st::vwire::KindHalfExtents(k, half);
        CHECK(std::fabs(p[0]) < half[0] && std::fabs(p[1]) < half[1] && p[2] >= 0 && p[2] < 2 * half[2]);
        CHECK(door[0] > 0.5f);
    }
    // roadster and truck: the passenger is the driver mirrored across the centre plane (HLP_Passenger = HLP_Driver mirrored)
    for (std::uint8_t k = 1; k <= 2; ++k) {
        PassengerSeat(k, p), DriverSeat(k, d);
        CHECK(p[0] > 0 && d[0] < 0 && std::fabs(p[0] + d[0]) < 1e-6f && p[1] == d[1] && p[2] == d[2]);
        CHECK(SeatOf(k) == Seat::passenger);
    }
    // the trike: a pillion behind the driver, on the centre line
    PassengerSeat(3, p), DriverSeat(3, d);
    CHECK(SeatOf(3) == Seat::pillion && p[1] < d[1] - 0.2f && std::fabs(p[0]) < 0.05f);
    CHECK(!PassengerSeat(0, p) && !PassengerSeat(4, p) && SeatOf(4) == Seat::none && !DoorPoint(9, door));

    // to the world and back, with a vehicle turned 90 deg (right = +y, forward = -x)
    const double pos[3] = {100, 200, 10};
    const float rows[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    const float off[3] = {1, 2, 3};
    double w[3], l[3];
    ToWorld(pos, rows, off, w);
    CHECK(std::fabs(w[0] - 98) < 1e-9 && std::fabs(w[1] - 201) < 1e-9 && std::fabs(w[2] - 13) < 1e-9);
    ToLocal(pos, rows, w, l);
    CHECK(std::fabs(l[0] - 1) < 1e-6 && std::fabs(l[1] - 2) < 1e-6 && std::fabs(l[2] - 3) < 1e-6);
}

void TestBoarding() {
    BoardIn in;
    in.feature_on = true, in.link_up = true, in.partner_drives = true, in.kind = 2, in.local_on_foot = true, in.door_dist_m = 1.0, in.vehicle_speed = 0.2;
    CHECK(BoardCheck(in) == Block::ok);
    BoardIn b = in;
    b.feature_on = false;
    CHECK(BoardCheck(b) == Block::off);
    b = in, b.link_up = false;
    CHECK(BoardCheck(b) == Block::no_link);
    b = in, b.partner_drives = false;
    CHECK(BoardCheck(b) == Block::no_vehicle);
    b = in, b.kind = 0;
    CHECK(BoardCheck(b) == Block::no_seat);
    b = in, b.local_on_foot = false;
    CHECK(BoardCheck(b) == Block::not_on_foot);
    b = in, b.busy = true;
    CHECK(BoardCheck(b) == Block::busy);
    b = in, b.saving = true;  // a save pending or being written: no boarding (save_guard)
    CHECK(BoardCheck(b) == Block::saving);
    CHECK(std::strcmp(BlockName(Block::saving), "the game is saving") == 0);
    b.busy = true;
    CHECK(BoardCheck(b) == Block::busy);
    b = in, b.door_dist_m = 2.5;
    CHECK(BoardCheck(b) == Block::ok);
    b = in, b.door_dist_m = 2.51;
    CHECK(BoardCheck(b) == Block::too_far);
    b = in, b.door_dist_m = std::nan("");
    CHECK(BoardCheck(b) == Block::too_far);
    b = in, b.vehicle_speed = 1.0;
    CHECK(BoardCheck(b) == Block::moving);
    b = in, b.vehicle_speed = -1.5;
    CHECK(BoardCheck(b) == Block::moving);
    // the game's own "Use Vehicle" prompt: from any side, within kNativeReachM of the vehicle's origin (the door ignored)
    b = in, b.native = true, b.door_dist_m = 6.0, b.origin_dist_m = 7.0;
    CHECK(BoardCheck(b) == Block::ok);
    b.origin_dist_m = 7.01;
    CHECK(BoardCheck(b) == Block::too_far);
    b.origin_dist_m = std::nan("");
    CHECK(BoardCheck(b) == Block::too_far);
    b = in, b.native = true, b.origin_dist_m = 2, b.vehicle_speed = 1.2;
    CHECK(BoardCheck(b) == Block::moving);  // the native prompt stays crossed out on a moving vehicle
    b = in, b.native = true, b.origin_dist_m = 2, b.busy = true;
    CHECK(BoardCheck(b) == Block::busy);

    // the driver's answer
    DriverIn d;
    d.feature_on = true, d.we_drive_it = true, d.speed = 0.5, d.have_partner = true, d.partner_dist_m = 3;
    Deny why = Deny::off;
    CHECK(DriverAccepts(d, &why) && why == Deny::count);
    DriverIn x = d;
    x.feature_on = false;
    CHECK(!DriverAccepts(x, &why) && why == Deny::off);
    x = d, x.same_world = false;
    CHECK(!DriverAccepts(x, &why) && why == Deny::world);
    x = d, x.we_drive_it = false;
    CHECK(!DriverAccepts(x, &why) && why == Deny::not_driving);
    x = d, x.seat_taken = true;
    CHECK(!DriverAccepts(x, &why) && why == Deny::seat_taken);
    x = d, x.speed = 2.0;
    CHECK(!DriverAccepts(x, &why) && why == Deny::moving);
    x = d, x.have_partner = false;
    CHECK(!DriverAccepts(x, &why) && why == Deny::not_near);
    x = d, x.partner_dist_m = 5.01;
    CHECK(!DriverAccepts(x, &why) && why == Deny::not_near);
    // a native request: the partner near the vehicle from any side
    x = d, x.native = true, x.partner_dist_m = 7.0, x.partner_origin_dist_m = 8.5;
    CHECK(DriverAccepts(x, &why) && why == Deny::count);
    x.partner_origin_dist_m = 8.6;
    CHECK(!DriverAccepts(x, &why) && why == Deny::not_near);
    x.partner_dist_m = 4.0;  // the door rule still accepts
    CHECK(DriverAccepts(x, &why));
    x = d, x.native = true, x.partner_origin_dist_m = 2.0, x.speed = 2.5;
    CHECK(!DriverAccepts(x, &why) && why == Deny::moving);
    CHECK(DriverAccepts(d, nullptr));

    // the driver's body placement and timeout
    CHECK(!DriverPlacesBody(false, 0, 0));
    CHECK(DriverPlacesBody(true, 100, 1e9));        // just accepted: the partner's pose follows within the grace
    CHECK(DriverPlacesBody(true, 10000, 200));      // its pose says passenger
    CHECK(!DriverPlacesBody(true, 10000, 1500));    // it stopped saying so
    CHECK(!DriverTimedOut(true, 4000, 1e9));
    CHECK(DriverTimedOut(true, 5000, 5000));
    CHECK(!DriverTimedOut(true, 9000, 300));
    CHECK(!DriverTimedOut(false, 9000, 9000));
}

void TestForced() {
    ForcedIn ok;
    CHECK(ForcedExit(ok) == End::count);
    ForcedIn f = ok;
    f.link_silent_ms = 1000;
    CHECK(ForcedExit(f) == End::count);
    f.link_silent_ms = 1001;
    CHECK(ForcedExit(f) == End::count);  // v0.21: a stall is ridden out while the link is up
    f.link_lost = true;
    CHECK(ForcedExit(f) == End::link);
    f.link_silent_ms = 900;
    CHECK(ForcedExit(f) == End::count);  // the link's own state alone: not before a second of silence
    f.link_lost = false, f.link_silent_ms = kLinkSilentCarriedMs;
    CHECK(ForcedExit(f) == End::count);
    f.link_silent_ms = kLinkSilentCarriedMs + 1;
    CHECK(ForcedExit(f) == End::link);
    f = ok, f.not_driven_ms = 401;
    CHECK(ForcedExit(f) == End::driver_off);
    f = ok, f.vehicle_live = false;
    CHECK(ForcedExit(f) == End::vehicle_gone);
    f = ok, f.broken = true;
    CHECK(ForcedExit(f) == End::broken);
    f = ok, f.flipped = true;
    CHECK(ForcedExit(f) == End::flipped);
    f = ok, f.crash_dv = 8.0;
    CHECK(ForcedExit(f) == End::count);
    f.crash_dv = 8.5;
    CHECK(ForcedExit(f) == End::crash);
    f = ok, f.driver_not_seated = true, f.speed = 1.0;
    CHECK(ForcedExit(f) == End::count);
    f.speed = 3.0;
    CHECK(ForcedExit(f) == End::not_seated);
    f = ok, f.taken = true;
    CHECK(ForcedExit(f) == End::taken);
    f = ok, f.world_changed = true;
    CHECK(ForcedExit(f) == End::world);
    f = ok, f.driver_ended = true;
    CHECK(ForcedExit(f) == End::driver_ended);
    f = ok, f.saving = true;
    CHECK(ForcedExit(f) == End::saving);
    CHECK(std::strcmp(EndName(static_cast<std::uint8_t>(End::saving)), "the game saves") == 0);
    f.taken = true;  // the game has Sam: its own placement, not ours
    CHECK(ForcedExit(f) == End::taken);
    f.taken = false, f.link_silent_ms = 5000, f.crash_dv = 20, f.driver_ended = true;  // the save first: it must not capture him carried
    CHECK(ForcedExit(f) == End::saving);
    // the order: the mod's and the game's reasons first, then the link, then the vehicle
    f = ok, f.stopped = true, f.world_changed = true, f.taken = true, f.link_silent_ms = 5000, f.crash_dv = 20;
    CHECK(ForcedExit(f) == End::stopped);
    f.stopped = false;
    CHECK(ForcedExit(f) == End::world);
    f.world_changed = false;
    CHECK(ForcedExit(f) == End::taken);
    f.taken = false;
    CHECK(ForcedExit(f) == End::link);
    f.link_silent_ms = 0;
    CHECK(ForcedExit(f) == End::crash);
    f = ok, f.crash_dv = 9, f.flipped = true;
    CHECK(ForcedExit(f) == End::crash);
}

void TestDetectors() {
    {
        // get off by a press (native prompt): one press after a release; the boarding press still held never counts
        PressDetector p;
        CHECK(!p.Update(true));
        CHECK(!p.Update(true));
        CHECK(!p.Update(false));
        CHECK(p.Update(true));
        CHECK(!p.Update(true));  // once per press
        CHECK(!p.Update(false));
        CHECK(p.Update(true));
        p.Reset();
        CHECK(!p.Update(true));
        CHECK(!p.Update(false));
        CHECK(p.Update(true));
        // the game's TryStart repeats while held: a new press only after a gap
        CHECK(NativePressIsNew(-1e18, 0));
        CHECK(!NativePressIsNew(1000, 1100));
        CHECK(!NativePressIsNew(1000, 1399));
        CHECK(NativePressIsNew(1000, 1400));
    }
    HoldDetector h;
    // a button already down when it starts never counts until released
    CHECK(!h.Update(true, 0));
    CHECK(!h.Update(true, 1000));
    CHECK(!h.Update(false, 1100));
    CHECK(!h.Update(true, 1200));
    CHECK(!h.Update(true, 1799));
    CHECK(h.Update(true, 1800));   // 600 ms
    CHECK(!h.Update(true, 2500));  // once
    CHECK(!h.Update(false, 2600));
    CHECK(!h.Update(true, 2700));
    CHECK(!h.Update(false, 2800));  // a tap
    CHECK(!h.Update(true, 2900));
    CHECK(h.Update(true, 3500));
    h.Reset();
    CHECK(!h.Update(true, 4000));
    CHECK(!h.Update(true, 5000));  // still held from before the reset
    CHECK(!h.Update(false, 5100));
    CHECK(!h.Update(true, 5200));
    CHECK(h.HeldMs(5500) == 300);
    CHECK(h.Update(true, 5800));

    FlipDetector fl;
    CHECK(!fl.Update(1.0, 0));
    CHECK(!fl.Update(0.2, 100));
    CHECK(!fl.Update(-0.5, 599));
    CHECK(fl.Update(-0.5, 600));
    CHECK(!fl.Update(0.31, 700));  // upright again
    CHECK(!fl.Update(0.1, 800));
    CHECK(!fl.Update(std::nan(""), 1400));  // not a reading: not flipped
    CHECK(!fl.Update(0.1, 1500));
    CHECK(fl.Update(0.1, 2000));

    CrashDetector c;
    const float v0[3] = {15, 0, 0}, v1[3] = {14, 0.5f, 0}, v2[3] = {3, 0, 0}, v3[3] = {3, 0, 0};
    CHECK(c.Update(v0, 1000) == 0);
    CHECK(c.Update(v1, 1033) == 0);  // braking
    CHECK(c.Update(v2, 1066) > 8);   // 11 m/s in one frame
    CHECK(c.Update(v3, 1066) == 0);  // the same key again
    CHECK(c.Update(v0, 1050) == 0);  // older
    CHECK(c.Update(v0, 1300) == 0);  // after a gap: starts over
    CHECK(c.Update(v2, 1500) == 0);  // 200 ms later: not one frame
    const float bad[3] = {std::nanf(""), 0, 0};
    CHECK(c.Update(bad, 1530) == 0);
    c.Reset();
    CHECK(c.Update(v2, 10) == 0);

    // v0.21: the confirmed crash
    {
        // play test 3: a steady 11 m/s with one odd frame (dv 9.8): never a crash, reported as rejected
        CrashConfirm k;
        double t = 0, fired = 0;
        for (int i = 0; i < 30; ++i, t += 33.3) {
            float v[3] = {11.0f, 0.3f, -1.0f};
            if (i == 15) v[0] = 1.2f;
            fired += k.Update(v, 40.0f, t);
        }
        CHECK(fired == 0);
        CHECK(k.TakeRejected() > 8);
        CHECK(k.TakeRejected() == 0);
        // a steady drive with a vertical jolt (a crest): not a crash
        CrashConfirm z;
        fired = 0, t = 0;
        for (int i = 0; i < 30; ++i, t += 33.3) {
            const float v[3] = {11.0f, 0, i >= 15 ? 9.0f : -1.0f};
            fired += z.Update(v, 40.0f, t);
        }
        CHECK(fired == 0);
        // a real crash: 11 m/s to 1 m/s and it stays, the speed field drops: fires once
        CrashConfirm r;
        int fires = 0;
        t = 0;
        for (int i = 0; i < 40; ++i, t += 33.3) {
            const float v[3] = {i < 15 ? 11.0f : 1.0f, 0, 0};
            if (r.Update(v, i < 15 ? 40.0f : 3.0f, t) > 8) ++fires;
        }
        CHECK(fires == 1);
        // the same velocity change while the game's speed field does not drop (a measurement artefact): no crash
        CrashConfirm s;
        fires = 0, t = 0;
        for (int i = 0; i < 40; ++i, t += 33.3) {
            const float v[3] = {i < 15 ? 11.0f : 1.0f, 0, 0};
            if (s.Update(v, 40.0f, t) > 8) ++fires;
        }
        CHECK(fires == 0);
        // the speed field unknown: the velocities decide
        CrashConfirm u;
        fires = 0, t = 0;
        for (int i = 0; i < 40; ++i, t += 33.3) {
            const float v[3] = {i < 15 ? 11.0f : 1.0f, 0, 0};
            if (u.Update(v, -1.0f, t) > 8) ++fires;
        }
        CHECK(fires == 1);
        // a hard brake (11 -> 0 over 1.5 s): not a crash
        CrashConfirm b;
        fires = 0, t = 0;
        for (int i = 0; i < 60; ++i, t += 33.3) {
            const float sp = i < 10 ? 11.0f : (std::max)(0.0f, 11.0f - 0.25f * (i - 10));
            const float v[3] = {sp, 0, 0};
            if (b.Update(v, sp * 3.6f, t) > 8) ++fires;
        }
        CHECK(fires == 0);
        // NaN, older keys
        CrashConfirm q;
        const float nanv[3] = {std::nanf(""), 0, 0};
        CHECK(q.Update(nanv, 10, 0) == 0);
        const float ok[3] = {1, 0, 0};
        CHECK(q.Update(ok, 10, 100) == 0);
        CHECK(q.Update(ok, 10, 50) == 0);
    }
}

void TestExit() {
    const double pos[3] = {0, 0, 5};
    const float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const float still[3] = {0, 0, 0};
    float half[3];
    for (std::uint8_t k = 1; k <= 3; ++k) {
        st::vwire::KindHalfExtents(k, half);
        // by choice: on the passenger's (right) side, beside the seat row, at the vehicle's height, facing it
        const ExitSpot s = ExitPlacement(k, pos, rows, still, false);
        CHECK(s.side == 1);
        CHECK(std::fabs(s.pos[0] - (half[0] + 0.7)) < 1e-5 && s.pos[2] == 5);
        CHECK(std::fabs(s.facing[0] + 1) < 1e-6 && std::fabs(s.facing[1]) < 1e-6);
        float door[3];
        DoorPoint(k, door);
        CHECK(std::fabs(s.pos[1] - door[1]) < 1e-5);
    }
    // forced, sliding right: put off on the left
    const float slide_right[3] = {4, 10, 0};
    ExitSpot s = ExitPlacement(2, pos, rows, slide_right, true);
    st::vwire::KindHalfExtents(2, half);
    CHECK(s.side == -1 && s.pos[0] < -half[0] && s.facing[0] > 0.99f);
    // forced, fast forward: moved back (at most 3 m)
    const float fast[3] = {0, 30, 0};
    s = ExitPlacement(2, pos, rows, fast, true);
    float door[3];
    DoorPoint(2, door);
    CHECK(s.side == 1 && std::fabs(s.pos[1] - (door[1] - 3.0)) < 1e-5);
    const float slow_back[3] = {0, -4, 0};
    s = ExitPlacement(2, pos, rows, slow_back, true);
    CHECK(std::fabs(s.pos[1] - (door[1] + 1.0)) < 1e-5);
    // by choice the velocity does not move it
    s = ExitPlacement(2, pos, rows, fast, false);
    CHECK(s.side == 1 && std::fabs(s.pos[1] - door[1]) < 1e-5);
    // a turned vehicle: right = +y
    const float turned[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    s = ExitPlacement(1, pos, turned, still, false);
    st::vwire::KindHalfExtents(1, half);
    DoorPoint(1, door);
    CHECK(std::fabs(s.pos[1] - (half[0] + 0.7)) < 1e-5 && std::fabs(s.pos[0] - (-door[1])) < 1e-5);
    CHECK(std::fabs(s.facing[1] + 1) < 1e-6);

    // rolled over (the flip exit): never under or inside the wreck, never on it; beside its body, at its lowest point
    for (std::uint8_t k = 1; k <= 3; ++k) {
        st::vwire::KindHalfExtents(k, half);
        DoorPoint(k, door);
        const double h = 2.0 * half[2];
        // on its right side: right = down, up = +x (the roof toward +x, the underside at x 0)
        const float right_side[9] = {0, 0, -1, 0, 1, 0, 1, 0, 0};
        for (const float* v : {still, slide_right}) {
            const float vel[3] = {v[0], v[1], v[2]};
            s = ExitPlacement(k, pos, right_side, vel, true);
            const bool beside = s.pos[0] >= h + 0.7 - 1e-5 || s.pos[0] <= -0.7 + 1e-5;  // past the roof or the underside
            CHECK(beside);
            CHECK(std::fabs(s.pos[2] - (5 - half[0])) < 1e-5);  // its right side's height (lying on it), not 2 m under the origin
            CHECK(std::fabs(s.facing[0]) > 0.99f && std::fabs(s.facing[1]) < 1e-6);
            CHECK((s.pos[0] > 0) == (s.facing[0] < 0));  // facing the vehicle
        }
        s = ExitPlacement(k, pos, right_side, still, true);
        CHECK(std::fabs(s.pos[0] - (h + 0.7)) < 1e-5 && std::fabs(s.pos[1] - door[1]) < 1e-5);
        // on its left side: right = up, up = -x
        const float left_side[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
        s = ExitPlacement(k, pos, left_side, still, true);
        CHECK(std::fabs(s.pos[0] - 0.7) < 1e-5);  // beside its underside (the body lies toward -x)
        CHECK(std::fabs(s.pos[2] - (5 - half[0])) < 1e-5);
        CHECK(s.facing[0] < -0.99f);
        // on its roof: right = -x, up = down
        const float roof[9] = {-1, 0, 0, 0, 1, 0, 0, 0, -1};
        s = ExitPlacement(k, pos, roof, still, true);
        CHECK(std::fabs(s.pos[0] + (half[0] + 0.7)) < 1e-5);  // beside it on its (now left) passenger side
        CHECK(std::fabs(s.pos[2] - (5 - h)) < 1e-5);  // its roof's height under the base
        CHECK(s.facing[0] > 0.99f);
        // standing on its nose: forward = down, up = +y; right stays horizontal
        const float nose[9] = {1, 0, 0, 0, 0, -1, 0, 1, 0};
        s = ExitPlacement(k, pos, nose, still, true);
        CHECK(std::fabs(s.pos[0] - (half[0] + 0.7)) < 1e-5 && std::fabs(s.pos[2] - (5 - half[1])) < 1e-5);
        CHECK(std::isfinite(s.pos[1]));
    }
}

// The save guard's per-frame decision (save_guard.cpp's kick detour: the capture's hold).
void TestSaveGate() {
    SaveHoldIn idle;
    idle.settled_frames = 1000;
    CHECK(SaveGate(idle) == SaveStep::run);  // nothing pending
    SaveHoldIn p = idle;
    p.pending = true;
    CHECK(SaveGate(p) == SaveStep::run);  // nobody carried: the save runs at once (no delay)
    // carried: held, also on the first frame (held_ms 0) and just under the cap
    SaveHoldIn c = p;
    c.carried = true, c.settled_frames = 0, c.held_ms = -1;
    CHECK(SaveGate(c) == SaveStep::hold);
    c.held_ms = 0;
    CHECK(SaveGate(c) == SaveStep::hold);
    c.held_ms = kSaveHoldCapMs - 1;
    CHECK(SaveGate(c) == SaveStep::hold);
    // the cap: the save runs unchanged (never dropped)
    c.held_ms = kSaveHoldCapMs;
    CHECK(SaveGate(c) == SaveStep::run_capped);
    c.held_ms = 1e9;
    CHECK(SaveGate(c) == SaveStep::run_capped);
    // off the ride but the placement still waits (PlaceOn's rate limit): held
    SaveHoldIn pl = p;
    pl.placing = true, pl.settled_frames = 0, pl.held_ms = 100;
    CHECK(SaveGate(pl) == SaveStep::hold);
    // off and placed: held until kSaveSettleFrames frames in a row, then run
    SaveHoldIn s = p;
    s.held_ms = 200;
    for (unsigned f = 0; f < kSaveSettleFrames; ++f) {
        s.settled_frames = f;
        CHECK(SaveGate(s) == SaveStep::hold);
    }
    s.settled_frames = kSaveSettleFrames;
    CHECK(SaveGate(s) == SaveStep::run);
    // the post-load save is never held, carried or not
    SaveHoldIn pls = c;
    pls.held_ms = 0, pls.post_load = true;
    CHECK(SaveGate(pls) == SaveStep::run);
    // not pending: run, whatever the ride does (no hold without a save)
    SaveHoldIn np = c;
    np.pending = false, np.held_ms = 50;
    CHECK(SaveGate(np) == SaveStep::run);
    // just after a ride, our Sam's entity still in the vehicle's box (the game has him, no placement of ours): held, the
    // settled frames do not count meanwhile; capped at the cap like the rest
    SaveHoldIn box = p;
    box.in_vehicle = true, box.settled_frames = 1000, box.held_ms = 10;
    CHECK(SaveGate(box) == SaveStep::hold);
    box.held_ms = kSaveHoldCapMs;
    CHECK(SaveGate(box) == SaveStep::run_capped);
    // the cap is per capture: once released or capped (done), the same capture is never held again (a game's own wait
    // after the cap must not restart a 3 s hold), carried, placing or in the box
    SaveHoldIn d = c;
    d.done = true, d.held_ms = -1;
    CHECK(SaveGate(d) == SaveStep::run);
    d.placing = true, d.in_vehicle = true, d.settled_frames = 0;
    CHECK(SaveGate(d) == SaveStep::run);
    CHECK(std::strcmp(SaveStepName(SaveStep::hold), "hold") == 0);
    CHECK(std::strcmp(SaveStepName(SaveStep::run_capped), "run (cap)") == 0);
    // the END reason travels in STQ1: END "the game saves" round trips
    std::uint8_t buf[64];
    Msg m = Req();
    m.type = MsgType::end;
    m.reason = static_cast<std::uint8_t>(End::saving);
    const std::size_t n = EncodeMsg(m, buf, sizeof(buf));
    CHECK(n == kMessageSize);
    Msg o;
    CHECK(DecodeMsg(buf, n, o) && o.reason == static_cast<std::uint8_t>(End::saving));
}

// The guard's "placed" check: our Sam out of the ridden vehicle's box; the sleeping passenger's ground-safe height.
void TestSaveBoxAndSpot() {
    const float flat[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double origin[3] = {100, 200, 50};
    const std::uint8_t truck = 2, trike = 3, roadster = 1;
    auto at = [&](double r, double f, double u) {
        static double p[3];
        p[0] = origin[0] + r, p[1] = origin[1] + f, p[2] = origin[2] + u;
        return p;
    };
    // the truck's passenger seat (measured: 0.44, 0.80, 1.95) is inside; the guard's release spot (2.20, 0.80, 0.18) is not
    {
        const double* s = at(0.44, 0.80, 1.95);
        const double seat[3] = {s[0], s[1], s[2]};
        CHECK(InVehicleBox(truck, origin, flat, seat));
        const double* r = at(2.20, 0.80, 0.18);
        const double rel[3] = {r[0], r[1], r[2]};
        CHECK(!InVehicleBox(truck, origin, flat, rel));
    }
    // every kind's exit spot (ExitPlacement, upright, at rest) is outside its box; its seat inside
    for (std::uint8_t k : {roadster, truck, trike}) {
        const float zero[3] = {};
        const ExitSpot e = ExitPlacement(k, origin, flat, zero, false);
        const double sp[3] = {e.pos[0], e.pos[1], e.pos[2] + 0.5};
        CHECK(!InVehicleBox(k, origin, flat, sp));
        float seat[3];
        CHECK(PassengerSeat(k, seat));
        double w[3];
        ToWorld(origin, flat, seat, w);
        CHECK(InVehicleBox(k, origin, flat, w));
    }
    // a rotated truck (90 deg yaw: right = +y): the seat follows the rotation
    {
        const float yaw[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
        const float seat[3] = {0.44f, 0.80f, 1.95f};
        double w[3];
        ToWorld(origin, yaw, seat, w);
        CHECK(InVehicleBox(truck, origin, yaw, w));
        const double beside[3] = {origin[0], origin[1] + 2.3, origin[2] + 0.2};
        CHECK(!InVehicleBox(truck, origin, yaw, beside));
    }
    // far below or above: outside
    {
        const double* u = at(0, 0, -1.0);
        const double under[3] = {u[0], u[1], u[2]};
        CHECK(!InVehicleBox(truck, origin, flat, under));
        const double* o = at(0, 0, 5.0);
        const double over[3] = {o[0], o[1], o[2]};
        CHECK(!InVehicleBox(truck, origin, flat, over));
    }
    // the save-time height: flat ground: the exit spot's height + the lift
    {
        const double sp[3] = {origin[0] + 2.2, origin[1] + 0.8, origin[2]};
        CHECK(std::fabs(SaveSpotHeight(origin, flat, sp) - (origin[2] + kSaveSpotLiftM)) < 1e-6);
    }
    // a truck parked across a 15 deg slope, its passenger (right) side uphill: ExitPlacement's spot is under the slope there,
    // the save-time spot is lifted onto the vehicle's ground plane (plus the lift), never inside the hillside
    {
        const double a = 15.0 * 3.14159265358979 / 180.0;
        const float tilt[9] = {static_cast<float>(std::cos(a)), 0, static_cast<float>(std::sin(a)), 0, 1, 0,
                               static_cast<float>(-std::sin(a)), 0, static_cast<float>(std::cos(a))};
        const float zero[3] = {};
        const ExitSpot e = ExitPlacement(truck, origin, tilt, zero, false);
        CHECK(e.side == 1);
        const double ground = origin[2] + (e.pos[0] - origin[0]) * std::tan(a);  // the slope's plane under the spot
        CHECK(e.pos[2] < ground);                                                  // the bug: the exit spot is in the hill
        const double h = SaveSpotHeight(origin, tilt, e.pos);
        CHECK(h >= ground + kSaveSpotLiftM - 1e-3);
        // downhill (the left side): the exit spot's own height already clears the plane, nothing lowers it
        const double left[3] = {origin[0] - 2.2, origin[1], origin[2] - 0.2};
        CHECK(SaveSpotHeight(origin, tilt, left) >= left[2] + kSaveSpotLiftM - 1e-9);
        // a vehicle on its side (up z 0): no plane, the exit spot's height + the lift
        const float side[9] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
        const double sp[3] = {origin[0] + 3, origin[1], origin[2] - 1.5};
        CHECK(std::fabs(SaveSpotHeight(origin, side, sp) - (sp[2] + kSaveSpotLiftM)) < 1e-9);
    }
}

}  // namespace

// Round 30 (run 20261127-171443 leg 2): the board press without the game's prompt
void TestNoPrompt() {
    NoPromptIn in;
    in.native_on = true, in.pressed = true, in.allowed = true, in.allowed_for_ms = 5000, in.approach_dist_m = 0.4;
    CHECK(NoPromptPress(in) == NoPrompt::board);  // the defaults: no prompt, no game press, no earlier press
    NoPromptIn x = in;
    x.native_on = false;  // the ImGui hold prompt boards then
    CHECK(NoPromptPress(x) == NoPrompt::idle);
    x = in, x.pressed = false;
    CHECK(NoPromptPress(x) == NoPrompt::idle);
    x = in, x.allowed = false;
    CHECK(NoPromptPress(x) == NoPrompt::not_allowed);
    // the game's own prompt on the partner's vehicle: its press path, never a second REQUEST
    x = in, x.partner_prompt_age_ms = 0;
    CHECK(NoPromptPress(x) == NoPrompt::game_prompt);
    x.partner_prompt_age_ms = kNoPromptQuietMs - 1;
    CHECK(NoPromptPress(x) == NoPrompt::game_prompt);
    x.partner_prompt_age_ms = kNoPromptQuietMs;
    CHECK(NoPromptPress(x) == NoPrompt::board);
    // another vehicle's RideOn: the press is the game's get-on there
    x = in, x.other_prompt_age_ms = 100;
    CHECK(NoPromptPress(x) == NoPrompt::other_vehicle);
    // the game's TryStart took a press (this tick, or under 500 ms ago)
    x = in, x.game_press_now = true;
    CHECK(NoPromptPress(x) == NoPrompt::game_press);
    x = in, x.game_press_age_ms = kNoPromptGameMs - 1;
    CHECK(NoPromptPress(x) == NoPrompt::game_press);
    // the door rule, not the native 7 m: only beside the door point
    x = in, x.approach_dist_m = kBoardRadiusM + 0.01;
    CHECK(NoPromptPress(x) == NoPrompt::too_far);
    x.approach_dist_m = std::nan("");
    CHECK(NoPromptPress(x) == NoPrompt::too_far);
    // the game gets its chance first, and one press per second at most
    x = in, x.allowed_for_ms = 100;
    CHECK(NoPromptPress(x) == NoPrompt::too_soon);
    x = in, x.since_last_ms = 900;
    CHECK(NoPromptPress(x) == NoPrompt::too_soon);
    x.since_last_ms = kNoPromptGapMs;
    CHECK(NoPromptPress(x) == NoPrompt::board);
    // NaN ages never board
    x = in, x.partner_prompt_age_ms = std::nan("");
    CHECK(NoPromptPress(x) == NoPrompt::game_prompt);
    for (int i = 0; i <= static_cast<int>(NoPrompt::count); ++i) CHECK(NoPromptName(static_cast<NoPrompt>(i)) != nullptr);
}


int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestMessages();
    TestSeats();
    TestBoarding();
    TestForced();
    TestDetectors();
    TestExit();
    TestSaveGate();
    TestSaveBoxAndSpot();
    TestNoPrompt();
    if (g_fail) {
        std::printf("st_ride_wire_tests: %d of %d checks FAILED\n", g_fail, g_checks);
        return 1;
    }
    std::printf("st_ride_wire_tests: all %d checks passed\n", g_checks);
    return 0;
}
