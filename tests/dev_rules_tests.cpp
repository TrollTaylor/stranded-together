// Dev tools' pure part (native/mod/dev_rules.hpp): the real-save rule for every session kind, the vehicle catalogue pairs and
// spawn records, the weather presets, the god modes, the place labels. No game.
#include <cstdio>
#include <cstring>
#include <initializer_list>

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
}  // namespace

int main() {
    TestOff();
    TestRealSave();
    TestSandbox();
    TestVehicles();
    TestWeatherGod();
    TestPlaces();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
