// Dev tools' pure part (native/mod/dev_rules.hpp): the real-save rule for every session kind, the vehicle catalogue pairs and
// spawn records, the weather presets, the god modes, the place labels, the place landing, the map markers. No game.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "dev_rules.hpp"

using namespace st::devrules;

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

Session S(bool enabled, bool redirected, int role, bool host_save) {
    Session s;
    s.enabled = enabled;
    s.saves_redirected = redirected;
    s.role = role;
    s.host_save = host_save;
    return s;
}

void TestOff() {
    // the dev tools off: everything refused with the launcher hint, whatever the save
    for (Tool t : {Tool::player, Tool::world, Tool::vehicle_spawn, Tool::vehicle_edit}) {
        const Verdict v = Decide(t, S(false, true, 0, false));
        CHECK(!v.ok() && std::strcmp(v.why, kOffReason) == 0);
    }
}

void TestRealSave() {
    // a real save (solo / echo, the online host with host_save=1, a host_save=0 host or guest on its own saves): player tools
    // only; every tool that changes what a save keeps is refused with the reason
    const Session real[] = {S(true, false, 0, false), S(true, false, 1, true), S(true, false, 1, false), S(true, false, 2, false)};
    for (const Session& s : real) {
        CHECK(Decide(Tool::player, s).route == Route::local);
        for (Tool t : {Tool::world, Tool::vehicle_spawn, Tool::vehicle_edit}) {
            const Verdict v = Decide(t, s);
            CHECK(!v.ok() && std::strcmp(v.why, kRealSaveReason) == 0);
        }
    }
}

void TestSandbox() {
    // solo on a test save / hidden run: everything here
    const Session solo = S(true, true, 0, false);
    for (Tool t : {Tool::player, Tool::world, Tool::vehicle_spawn, Tool::vehicle_edit}) CHECK(Decide(t, solo).route == Route::local);
    // the Local test with host_save=1: the host's vehicles are shared (M2), its weather / enemies are its own world's
    const Session host = S(true, true, 1, true);
    CHECK(Decide(Tool::vehicle_spawn, host).route == Route::local);
    CHECK(Decide(Tool::vehicle_edit, host).route == Route::local);
    CHECK(Decide(Tool::world, host).route == Route::local_only);
    // its guest: a vehicle is asked of the host (which decides), an existing one is the host's to change, the rest is the
    // guest's own copy of the world
    const Session guest = S(true, true, 2, true);
    CHECK(Decide(Tool::vehicle_spawn, guest).route == Route::ask_host);
    const Verdict edit = Decide(Tool::vehicle_edit, guest);
    CHECK(!edit.ok() && std::strcmp(edit.why, kGuestEditReason) == 0);
    CHECK(Decide(Tool::world, guest).route == Route::local_only);
    CHECK(Decide(Tool::player, guest).route == Route::local);
    // a co-op guest's spawn goes to the host even if it were not redirected (DllMain refuses that session anyway); the host's
    // rule decides it
    CHECK(Decide(Tool::vehicle_spawn, S(true, false, 2, true)).route == Route::ask_host);
    // host_save=0 (two separate worlds, the old Local test): each game's own world
    for (int role : {1, 2})
        for (Tool t : {Tool::world, Tool::vehicle_spawn, Tool::vehicle_edit}) CHECK(Decide(t, S(true, true, role, false)).route == Route::local_only);
    // structures (dev_build; docs/world-sync.md): sandboxed saves only; the co-op guest asks the host
    CHECK(Decide(Tool::structure, solo).route == Route::local);
    CHECK(Decide(Tool::structure, host).route == Route::local);
    CHECK(Decide(Tool::structure, guest).route == Route::ask_host);
    for (const Session& s : {S(true, false, 0, false), S(true, false, 1, true)}) {
        const Verdict v = Decide(Tool::structure, s);
        CHECK(!v.ok() && std::strcmp(v.why, kRealSaveReason) == 0);
    }
    CHECK(!Decide(Tool::structure, S(false, true, 1, true)).ok());
    // the host's answer to a guest's dev request
    CHECK(HostAllowsDevRequest(true));
    CHECK(!HostAllowsDevRequest(false));
}

void TestVehicles() {
    static_assert(kVehiclePairCount == 14, "the game's table 0x144D7F880: 14 entries of {item id, kind, variant}");
    int per_kind[4] = {};
    for (const VehiclePair& p : kVehiclePairs) {
        CHECK(p.kind >= 1 && p.kind <= 3);
        CHECK(ValidVehicle(p.kind, p.variant));
        ++per_kind[p.kind];
        const std::uint16_t r = VehicleRec0(p.kind, p.variant);
        CHECK((r & 0xF) == p.kind && ((r >> 6) & 0x3F) == p.variant);
    }
    CHECK(per_kind[1] == 2 && per_kind[2] == 6 && per_kind[3] == 6);
    CHECK(!ValidVehicle(1, 2));
    CHECK(!ValidVehicle(2, 3));
    CHECK(!ValidVehicle(3, 4));
    CHECK(!ValidVehicle(0, 0));
    CHECK(!ValidVehicle(4, 0));
    CHECK(VehicleRec0(3, 7) == 0x1C3);  // the 100% save's Kusanagi-like trike (unit flags 0x01E005 >> 22 = 7)
    CHECK(std::strcmp(KindName(2), "truck") == 0 && std::strcmp(KindName(9), "?") == 0);
    double out[3];
    const double here[3] = {10, 20, 30};
    Ahead(here, 0, 1, 8, out);
    CHECK(out[0] == 10 && out[1] == 28 && out[2] == 30.5);
}

void TestWeatherGod() {
    Weather w;
    CHECK(WeatherFromCode(0, &w) && w == Weather::clear && static_cast<int>(w) == 6);
    CHECK(WeatherFromCode(1, &w) && w == Weather::rain && static_cast<int>(w) == 3);
    CHECK(WeatherFromCode(2, &w) && w == Weather::heavy && static_cast<int>(w) == 5);
    CHECK(WeatherFromCode(3, &w) && w == Weather::normal && static_cast<int>(w) == 2);
    CHECK(!WeatherFromCode(4, &w) && !WeatherFromCode(-1, &w));
    static_assert(kWeatherRegions == 64, "EDSWeatherRegionIndex");
    CHECK(ValidGodMode(0) && ValidGodMode(2) && !ValidGodMode(3) && !ValidGodMode(-1));
    CHECK(std::strcmp(GodModeName(2), "invulnerable") == 0);
}

void TestPlaces() {
    char b[48];
    CHECK(PlaceLabel("Capital Knot City", 0, 3, b, sizeof(b)) == std::strlen("Capital Knot City") && std::strcmp(b, "Capital Knot City") == 0);
    PlaceLabel(nullptr, 1, 12, b, sizeof(b));
    CHECK(std::strcmp(b, "prepper shelter #12") == 0);
    PlaceLabel("", 99, 1, b, sizeof(b));
    CHECK(std::strcmp(b, "place #1") == 0);
    char tiny[6];
    CHECK(PlaceLabel("Distribution Center", 0, 0, tiny, sizeof(tiny)) == 5 && std::strcmp(tiny, "Distr") == 0);
    CHECK(PrintableName("Lake Knot City", 64));
    CHECK(!PrintableName("", 64));
    CHECK(!PrintableName("bad\x01", 64));
    CHECK(!PrintableName("\xC3\xA9t\xC3\xA9", 64));  // UTF-8 (not ASCII): shown as the type name instead
}
void TestGoto() {
    CHECK(GotoTargetOk(-1100.09, 270.64, 352.49) && GotoTargetOk(1, 2, 0));
    CHECK(!GotoTargetOk(60000, 0, 0) && !GotoTargetOk(0, 0, 20000) && !GotoTargetOk(std::numeric_limits<double>::infinity(), 0, 0));
    // a known z: still for 1.5 s at the target settles; moving restarts the window
    GotoTrack t;
    t.x = 100, t.y = 200, t.z = 50, t.t0 = 0, t.last_place = 0;
    double p[3] = {100.5, 200.5, 50.2};
    CHECK(GotoAdvance(t, true, p, 100) == GotoStep::wait);
    CHECK(GotoAdvance(t, true, p, 1000) == GotoStep::wait);
    p[2] = 51.0;  // moved 0.8 m: the window restarts
    CHECK(GotoAdvance(t, true, p, 1200) == GotoStep::wait);
    CHECK(GotoAdvance(t, true, p, 2600) == GotoStep::wait);
    CHECK(GotoAdvance(t, true, p, 2800) == GotoStep::settled);
    // not playable: never settles; the timeout after 25 s
    GotoTrack u = t;
    u.still_since = -1;
    CHECK(GotoAdvance(u, false, p, 10000) == GotoStep::wait);
    CHECK(GotoAdvance(u, false, p, 26000) == GotoStep::timeout);
    // fell through the ground: placed again (at most 4 placements, 1 s apart), never settled down there
    GotoTrack f;
    f.x = 0, f.y = 0, f.z = 300, f.t0 = 0, f.last_place = 0;
    double low[3] = {0.5, 0.5, 280};
    CHECK(GotoAdvance(f, true, low, 500) == GotoStep::wait);  // 1 s after the last placement only
    CHECK(GotoAdvance(f, true, low, 1000) == GotoStep::place_again && f.placements == 2);
    CHECK(GotoAdvance(f, true, low, 1500) == GotoStep::wait);
    CHECK(GotoAdvance(f, true, low, 2000) == GotoStep::place_again);
    CHECK(GotoAdvance(f, true, low, 3000) == GotoStep::place_again && f.placements == 4);
    CHECK(GotoAdvance(f, true, low, 4500) == GotoStep::wait);
    CHECK(GotoAdvance(f, true, low, 9000) == GotoStep::wait);  // still too low: no settle
    // far from x, y (the teleport did not take): no settle
    GotoTrack g;
    g.x = 0, g.y = 0, g.z = 0;
    double away[3] = {30, 0, 5};
    CHECK(GotoAdvance(g, true, away, 100) == GotoStep::wait && GotoAdvance(g, true, away, 2000) == GotoStep::wait);
    // the ground (z 0): any height settles
    double there[3] = {1, 1, 812.4};
    CHECK(GotoAdvance(g, true, there, 3000) == GotoStep::wait && GotoAdvance(g, true, there, 4600) == GotoStep::settled);
    // fell: only with a known z (the way back on a timeout uses it too); never for z 0
    CHECK(GotoFell(f, low) && !GotoFell(g, there));
    const double ok_z[3] = {0, 0, 297};
    CHECK(!GotoFell(f, ok_z));
    // snap (the topmost ground at a reference height): 30 m either way is "at"; 31 m below fell
    GotoTrack sn;
    sn.x = 0, sn.y = 0, sn.z = 300, sn.snap = true;
    const double roof[3] = {0, 0, 320}, drop[3] = {0, 0, 278}, deep[3] = {0, 0, 269};
    CHECK(!GotoFell(sn, roof) && !GotoFell(sn, drop) && GotoFell(sn, deep));
    CHECK(GotoAdvance(sn, true, drop, 100) == GotoStep::wait && GotoAdvance(sn, true, drop, 1700) == GotoStep::settled);
    float r = -1;
    CHECK(BootsRate(100, &r) && r == 1.0f && BootsRate(0, &r) && r == 0.0f && BootsRate(42.5, &r) && r > 0.42f && r < 0.43f);
    CHECK(!BootsRate(101, &r) && !BootsRate(-1, &r));
}
void TestLanding() {
    // the kind filter: 0 any, -1 delivery bases, 5 every post box (5..7), 20 MULE posts, 3 every safe house (2..4)
    CHECK(PlaceFilterMatches(0, 36) && PlaceFilterMatches(-1, 0) && !PlaceFilterMatches(-1, 1));
    CHECK(PlaceFilterMatches(5, 5) && PlaceFilterMatches(5, 7) && !PlaceFilterMatches(5, 20) && PlaceFilterMatches(20, 20));
    CHECK(PlaceFilterMatches(3, 2) && PlaceFilterMatches(2, 4) && !PlaceFilterMatches(2, 5));
    CHECK(SmallPost(5) && SmallPost(6) && SmallPost(7) && SmallPost(20) && !SmallPost(0) && !SmallPost(36));
    CHECK(LandsOnOwnPoint(0) && LandsOnOwnPoint(1) && LandsOnOwnPoint(4) && !LandsOnOwnPoint(5) && !LandsOnOwnPoint(20) && !LandsOnOwnPoint(-1));
    // the game's front access math: p + o.x * right + o.y * forward + o.z * up (a structure turned 90 degrees: forward = -x)
    const double p[3] = {100, 200, 50};
    const float rot[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};  // right (0, 1, 0), forward (-1, 0, 0), up
    const float off[3] = {0.5f, 2.0f, 0.25f};
    double s[3];
    FrontAccess(p, rot, off, s);
    CHECK(s[0] == 98.0 && s[1] == 200.5 && s[2] == 50.25);
    CHECK(StandPointOk(p, s));
    const double near[3] = {100.3, 200.2, 50}, far[3] = {120, 200, 50}, high[3] = {102, 200, 55}, nan[3] = {std::numeric_limits<double>::quiet_NaN(), 0, 0};
    CHECK(!StandPointOk(p, near) && !StandPointOk(p, far) && !StandPointOk(p, high) && !StandPointOk(p, nan));
    // the collision box: usable, the ring radius from its flat half-size; else 1.5 m (posts) / 4 m (others), + 1.2 m
    const float mn[3] = {-0.9f, -0.6f, 0}, mx[3] = {0.9f, 0.7f, 1.8f}, flat[3] = {0.9f, 0.7f, 0}, huge[3] = {80, 1, 1};
    CHECK(BoxUsable(mn, mx) && !BoxUsable(mn, flat) && !BoxUsable(mn, huge) && !BoxUsable(mx, mn));
    CHECK(FallbackRadius(5, true, mn, mx) > 2.0999 && FallbackRadius(5, true, mn, mx) < 2.1001);
    CHECK(FallbackRadius(20, false, mn, mx) == 1.5 + kFallbackClearM && FallbackRadius(0, false, mn, mx) == 4.0 + kFallbackClearM);
    // the plan: the stand point first, then the ring (forward, back, right, left) at the place's height
    LandPlan plan = PlanLanding(p, rot, true, s, 2.0);
    CHECK(plan.n == 6 && std::strcmp(plan.c[0].rule, "front access") == 0 && plan.c[0].pos[0] == 98.0 && !plan.c[0].snap);
    CHECK(std::strcmp(plan.c[1].rule, "fallback +forward") == 0 && plan.c[1].pos[0] == 98.0 && plan.c[1].pos[1] == 200.0 && plan.c[1].pos[2] == 50);
    CHECK(plan.c[2].pos[0] == 102.0 && plan.c[3].pos[1] == 202.0 && plan.c[4].pos[1] == 198.0 && !plan.c[4].snap);
    // the last: +forward at twice the radius, the topmost ground (a slope or a drop the ring's short probe missed)
    CHECK(plan.c[5].snap && plan.c[5].pos[0] == 96.0 && plan.c[5].pos[1] == 200.0 && plan.c[5].pos[2] == 50);
    plan = PlanLanding(p, rot, false, s, 2.0);
    CHECK(plan.n == 5 && std::strcmp(plan.c[0].rule, "fallback +forward") == 0 && plan.c[4].snap);
    const float zero[9] = {};
    plan = PlanLanding(p, zero, false, s, 3.0);  // no rotation: +y forward
    CHECK(plan.n == 5 && plan.c[0].pos[0] == 100 && plan.c[0].pos[1] == 203.0);
    CHECK(plan.n < kMaxLandings);  // room for dev_place's test candidate
    // the candidate walk: settled outside -> stay; inside -> the next while one is left, else give up
    CHECK(LandAfterSettle(false, 1, 5) == LandStep::stay && LandAfterSettle(false, 5, 5) == LandStep::stay);
    CHECK(LandAfterSettle(true, 1, 5) == LandStep::next && LandAfterSettle(true, 4, 5) == LandStep::next);
    CHECK(LandAfterSettle(true, 5, 5) == LandStep::give_up);
    // a fall: the first re-placement at the same point (the ground streams in), the second fall moves on; a fallen timeout moves
    // on; at the last candidate: placed again, a timeout gives up
    CHECK(LandAfterFall(2, false, 1, 5) == LandStep::stay && LandAfterFall(3, false, 1, 5) == LandStep::next);
    CHECK(LandAfterFall(1, true, 2, 5) == LandStep::next && LandAfterFall(3, false, 5, 5) == LandStep::stay);
    CHECK(LandAfterFall(4, true, 5, 5) == LandStep::give_up);
    // a walk over a plan where every ring point is inside: 5 placements, then give up (never loops)
    {
        int next = 1, placed = 1;
        LandStep st = LandStep::next;
        while ((st = LandAfterSettle(true, next, 5)) == LandStep::next) ++next, ++placed;
        CHECK(st == LandStep::give_up && placed == 5);
    }
    // inside the box: his middle (feet + 0.9 m) in the local box, shrunk 10 cm
    double local[3];
    const double at[3] = {100, 200, 50}, front[3] = {98, 200.2, 50}, side[3] = {100.2, 200.95, 50};
    CHECK(InsideBox(at, p, rot, mn, mx, local) && local[2] > 0.89 && local[2] < 0.91);
    CHECK(!InsideBox(front, p, rot, mn, mx, local) && local[1] > 1.99 && local[1] < 2.01);  // 2 m in front (local +y)
    CHECK(!InsideBox(side, p, rot, mn, mx, local));                                         // local x 0.95 > 0.9 - 0.1
}

void TestMarkers() {
    char name[4];
    MarkerName(2, 0, name);
    CHECK(std::strcmp(name, "C0") == 0);
    MarkerName(4, 9, name);
    CHECK(std::strcmp(name, "E9") == 0);
    MarkerName(5, 0, name);
    CHECK(std::strcmp(name, "??") == 0);
    CHECK(MarkerSlotOk(0, 0) && !MarkerSlotOk(-1, 0) && !MarkerSlotOk(0, 10));
    Marker m[3];
    m[0].id = 4, m[1].id = 9, m[2].id = 7;
    CHECK(PickMarker(m, 3, -1) == 1);  // the newest: the largest id
    CHECK(PickMarker(m, 3, 7) == 2);   // by id
    CHECK(PickMarker(m, 3, 5) == -1);  // gone (the game removes a reached one-marker route)
    CHECK(PickMarker(m, 0, -1) == -1);
    // ids start at 0 (run 183504: "marker A0: id 0"): id 0 is pickable, the action sends id + 1 (0 = the newest)
    m[0].id = 0;
    CHECK(PickMarker(m, 3, 0) == 0 && PickMarker(m, 3, -1) == 1);
    CHECK(MarkerWant(0) == -1 && MarkerWant(1) == 0 && MarkerWant(10) == 9 && MarkerWant(-3) == -1 && MarkerWant(0.5) == -1);
    // the newest on this map: the newest overall (9) is tagged with another area, 7 is here
    m[1].area = 100, m[2].area = 200, m[0].area = 200;
    CHECK(PickMarker(m, 3, -1, 200) == 2 && PickMarker(m, 3, -1, 100) == 1 && PickMarker(m, 3, -1, -1) == 1);
    m[0].area = m[2].area = 100;  // none here: the newest of all (its refusal names the other map)
    CHECK(PickMarker(m, 3, -1, 200) == 1);
    CHECK(KnownArea(200) == 200 && KnownArea(0xFFFF) == -1 && KnownArea(-1) == -1 && KnownArea(0) == 0);
    // z: the marker's own once the heightmap query is done (+0x42 clear), even with the raycast (+0x41) still pending
    m[0].pos[2] = 405.32, m[0].pending = 0;
    CHECK(MarkerTargetZ(m[0]) == 405.32);
    m[0].pending = 1;
    CHECK(MarkerTargetZ(m[0]) == 405.32);
    m[0].pending = 2;  // the heightmap query pending: the topmost ground
    CHECK(MarkerTargetZ(m[0]) == 0);
    m[0].pending = 3;
    CHECK(MarkerTargetZ(m[0]) == 0);
    m[0].pending = 0, m[0].pos[2] = 0;
    CHECK(MarkerTargetZ(m[0]) == 0);
    CHECK(MarkerAreaOk(-1, 200) && MarkerAreaOk(200, 200) && !MarkerAreaOk(100, 200) && MarkerAreaOk(200, -1));
    // the area tag's marker: the same id at the same place (another save's marker 3 in that slot is elsewhere)
    CHECK(SameMarker(3, 100, 200, 3, 100.5, 200) && !SameMarker(3, 100, 200, 3, 400, 200) && !SameMarker(3, 100, 200, 4, 100, 200));
    char area[64];
    AreaText(200, "Area02", area, sizeof(area));
    CHECK(std::strcmp(area, "the Central Region (Area02)") == 0);
    AreaText(300, "Area03", area, sizeof(area));
    CHECK(std::strcmp(area, "Area03") == 0);
    AreaText(123, nullptr, area, sizeof(area));
    CHECK(std::strcmp(area, "area 123") == 0);
    AreaText(-1, nullptr, area, sizeof(area));
    CHECK(std::strcmp(area, "an unknown area") == 0);
}
}  // namespace

int main() {
    TestOff();
    TestRealSave();
    TestSandbox();
    TestVehicles();
    TestWeatherGod();
    TestPlaces();
    TestGoto();
    TestLanding();
    TestMarkers();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
