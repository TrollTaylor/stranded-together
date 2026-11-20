// Debug-menu checks that need neither the game nor a GPU: the action registry
// and queue (threading), results, the status snapshot, the log ring, saved
// positions and key names, the game-state decoders and the diagnostics report.
// tests/overlay_sync_stubs.cpp stands in for player_sync and game_actions.
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "debug_actions.hpp"
#include "dev_rules.hpp"
#include "log.hpp"
#include "mouse_lock.hpp"
#include "overlay_keys.hpp"
#include "overlay_positions.hpp"
#include "player_sync.hpp"

// overlay_imconfig.h routes IM_ASSERT here (no ImGui code runs in this test).
void StOverlayImGuiAssert(const char*, const char*, int) {}

namespace st::sync::stub {
extern std::atomic<int> respawns, removes, visible_calls, log_positions, log_partner, tuning_calls;
extern std::atomic<unsigned long> last_thread;
extern float delay, offset;
extern bool verbose, have_partner;
}  // namespace st::sync::stub
namespace st::game::stub {
extern std::atomic<int> teleports, unstucks, heals, times, cycles, pings, ticks;
extern bool teleport_ok;
extern double last_target[3];
extern int last_mode;
extern float last_heal, last_hours;
extern bool last_cycle;
extern std::uintptr_t last_ping_entity;
extern int last_ping_index;
extern std::atomic<int> catalogs, surveys, censuses, real_cmds;
extern int last_real_op;
extern int last_catalog_start, last_catalog_count;
extern double last_catalog_distance;
}  // namespace st::game::stub
namespace st::dev {
extern const char* stub_real_save_refusal;
}  // namespace st::dev

namespace {
int g_failures = 0;
int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

using namespace st;

void TestRegistry() {
    std::size_t n = 0;
    const debug::ActionInfo* all = debug::Actions(&n);
    CHECK(n == static_cast<std::size_t>(debug::Action::count));
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(all[i].label && all[i].label[0]);
        CHECK(all[i].help && all[i].help[0]);
        CHECK(debug::Find(all[i].id) != nullptr);
        CHECK(std::strcmp(debug::ActionName(all[i].id), all[i].label) == 0);
    }
    // every Action value appears exactly once
    for (int a = 0; a < static_cast<int>(debug::Action::count); ++a) {
        int seen = 0;
        for (std::size_t i = 0; i < n; ++i) seen += static_cast<int>(all[i].id) == a;
        CHECK(seen == 1);
    }
    CHECK(debug::Find(debug::Action::count) == nullptr);
}

void TestQueueThreads() {
    debug::ResetForTest();
    const int before = sync::stub::respawns.load();
    std::vector<std::thread> producers;
    std::atomic<int> accepted{0};
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([&] {
            for (int i = 0; i < 10; ++i)
                if (debug::Queue(debug::Action::respawn_partner)) ++accepted;
        });
    for (auto& t : producers) t.join();
    CHECK(accepted.load() == 40);
    CHECK(debug::Pending() == 40);
    CHECK(sync::stub::respawns.load() == before);  // nothing runs before the tick drains

    unsigned long tick_thread = 0;
    std::size_t drained = 0;
    std::thread tick([&] {
        tick_thread = GetCurrentThreadId();
        drained = debug::Drain();
    });
    tick.join();
    CHECK(drained == 40);
    CHECK(debug::Pending() == 0);
    CHECK(sync::stub::respawns.load() == before + 40);
    CHECK(sync::stub::last_thread.load() == tick_thread);  // executed on the draining (tick) thread

    debug::Result r[64];
    CHECK(debug::RecentResults(r, 64) == 32);  // the result ring keeps the newest 32
    CHECK(debug::RecentResults(r, 5) == 5);
    CHECK(r[4].ok && r[4].action == debug::Action::respawn_partner);
}

void TestQueueFull() {
    debug::ResetForTest();
    int ok = 0;
    for (int i = 0; i < 64; ++i) ok += debug::Queue(debug::Action::log_status) != 0;
    CHECK(ok == 64);
    CHECK(debug::Queue(debug::Action::log_status) == 0);  // full: refused, never blocks
    CHECK(debug::Drain() == 64);
    CHECK(debug::Queue(debug::Action::log_status) != 0);
    debug::Drain();
    debug::Request bad;
    bad.action = debug::Action::count;
    CHECK(debug::Queue(bad) == 0);
}

bool LogHas(const char* needle) {
    std::vector<LogLine> lines(kLogRingLines);
    const std::size_t n = LogCopyRecent(0, lines.data(), lines.size());
    for (std::size_t i = 0; i < n; ++i)
        if (std::strstr(lines[i].text, needle)) return true;
    return false;
}

void TestActions() {
    debug::ResetForTest();
    // Teleport: every path ends in game::TeleportLocalPlayer (stubbed here).
    game::stub::teleport_ok = false;
    debug::Request t;
    t.action = debug::Action::teleport;
    t.v[0] = 1.5, t.v[1] = 2.5, t.v[2] = -3.5;
    const std::uint32_t s1 = debug::Queue(t);
    CHECK(s1 != 0);
    CHECK(!debug::ResultFor(s1, nullptr));
    debug::Drain();
    debug::Result r;
    CHECK(debug::ResultFor(s1, &r));
    CHECK(!r.ok);
    CHECK(std::strstr(r.text, "failed: not implemented") != nullptr);
    CHECK(LogHas("FAILED: teleport to (1.50, 2.50, -3.50) failed"));
    CHECK(game::stub::last_mode == static_cast<int>(game::TeleportMode::place));
    CHECK(!game::TeleportLocalPlayer(0, 0, 0));  // the 3-argument form = mode place

    game::stub::teleport_ok = true;
    t.i = static_cast<std::int32_t>(game::TeleportMode::snap_to_ground);
    t.v[2] = game::kSnapRayCentreZ;
    const std::uint32_t s1b = debug::Queue(t);
    debug::Drain();
    CHECK(debug::ResultFor(s1b, &r) && r.ok && std::strstr(r.text, "placed at (1.50, 2.50, 500.00)") != nullptr);
    CHECK(game::stub::last_mode == static_cast<int>(game::TeleportMode::snap_to_ground));

    sync::stub::have_partner = false;
    const std::uint32_t s2 = debug::Queue(debug::Action::teleport_to_partner);
    debug::Drain();
    CHECK(debug::ResultFor(s2, &r) && !r.ok && std::strstr(r.text, "no partner") != nullptr);
    sync::stub::have_partner = true;
    const std::uint32_t s3 = debug::Queue(debug::Action::teleport_to_partner);
    debug::Drain();
    CHECK(debug::ResultFor(s3, &r) && r.ok && std::strstr(r.text, "partner") != nullptr);
    CHECK(game::stub::last_mode == static_cast<int>(game::TeleportMode::place));
    CHECK(game::stub::last_target[0] == 1012.5 && game::stub::last_target[2] == -2210.75);
    game::stub::teleport_ok = false;
    const std::uint32_t s3b = debug::Queue(debug::Action::teleport_to_partner);
    debug::Drain();
    CHECK(debug::ResultFor(s3b, &r) && !r.ok);
    CHECK(LogHas("(1012.50, 48.25, -2210.75)"));

    // Game actions: one request each, run by Drain, a result line per action.
    const int heals0 = game::stub::heals.load(), times0 = game::stub::times.load(), cycles0 = game::stub::cycles.load(),
              pings0 = game::stub::pings.load(), unstuck0 = game::stub::unstucks.load();
    debug::Request h;
    h.action = debug::Action::heal;
    const std::uint32_t sh = debug::Queue(h);
    debug::Request tod;
    tod.action = debug::Action::set_time_of_day;
    tod.f[0] = 6.25f;
    const std::uint32_t st = debug::Queue(tod);
    tod.f[0] = 25.0f;
    const std::uint32_t st_bad = debug::Queue(tod);
    debug::Request cyc;
    cyc.action = debug::Action::set_day_night_cycle;
    cyc.flag = false;
    const std::uint32_t sc = debug::Queue(cyc);
    const std::uint32_t sp = debug::Queue(debug::Action::voice_ping);
    const std::uint32_t su = debug::Queue(debug::Action::unstuck);
    CHECK(debug::Drain() == 6);
    CHECK(game::stub::heals.load() == heals0 + 1 && game::stub::last_heal == 0.0f);
    CHECK(game::stub::times.load() == times0 + 2);
    CHECK(game::stub::cycles.load() == cycles0 + 1 && !game::stub::last_cycle);
    CHECK(game::stub::pings.load() == pings0 + 1 && game::stub::last_ping_index == -1);
    CHECK(game::stub::last_ping_entity == 0x7FF6A0002000ull);  // the live partner from the sync accessors
    CHECK(game::stub::unstucks.load() == unstuck0 + 1);
    CHECK(debug::ResultFor(sh, &r) && r.ok && std::strstr(r.text, "health") != nullptr);
    CHECK(debug::ResultFor(st, &r) && r.ok);
    CHECK(debug::ResultFor(st_bad, &r) && !r.ok);
    CHECK(debug::ResultFor(sc, &r) && r.ok && std::strstr(r.text, "frozen") != nullptr);
    CHECK(debug::ResultFor(sp, &r) && r.ok);
    CHECK(debug::ResultFor(su, &r) && !r.ok);
    CHECK(LogHas("FAILED: the game has no safe position recorded"));

    // The world time is world state a save may keep: refused on a real save (from every tab), the game is not called.
    dev::stub_real_save_refusal = devrules::kRealSaveReason;
    const int times1 = game::stub::times.load(), cycles1 = game::stub::cycles.load();
    tod.f[0] = 12.0f;
    const std::uint32_t st_real = debug::Queue(tod);
    const std::uint32_t sc_real = debug::Queue(cyc);
    CHECK(debug::Drain() == 2);
    CHECK(game::stub::times.load() == times1 && game::stub::cycles.load() == cycles1);
    CHECK(debug::ResultFor(st_real, &r) && !r.ok && std::strstr(r.text, "real save") != nullptr);
    CHECK(debug::ResultFor(sc_real, &r) && !r.ok && std::strstr(r.text, "real save") != nullptr);
    dev::stub_real_save_refusal = nullptr;

    sync::stub::have_partner = false;
    debug::Request ping;
    ping.action = debug::Action::voice_ping;
    ping.i = 3;
    const std::uint32_t sp2 = debug::Queue(ping);
    debug::Drain();
    CHECK(debug::ResultFor(sp2, &r) && !r.ok && std::strstr(r.text, "no live partner") != nullptr);
    CHECK(game::stub::last_ping_index == 2 && game::stub::last_ping_entity == 0);
    sync::stub::have_partner = true;
    game::stub::last_cycle = true;

    // Holo catalog: v[0] start, v[1] count (at least 1), v[2] distance.
    debug::Request cat;
    cat.action = debug::Action::holo_catalog;
    cat.v[0] = 45;
    cat.v[1] = 0;
    cat.v[2] = 4.5;
    const std::uint32_t scat = debug::Queue(cat);
    debug::Drain();
    CHECK(debug::ResultFor(scat, &r) && r.ok && std::strstr(r.text, "catalog") != nullptr);
    CHECK(game::stub::catalogs.load() == 1 && game::stub::last_catalog_start == 45 && game::stub::last_catalog_count == 1 &&
          std::fabs(game::stub::last_catalog_distance - 4.5) < 1e-9);
    const std::uint32_t ssur = debug::Queue(debug::Action::clip_survey);
    debug::Drain();
    CHECK(debug::ResultFor(ssur, &r) && r.ok && game::stub::surveys.load() == 1);
    const std::uint32_t scen = debug::Queue(debug::Action::anim_census);
    const std::uint32_t spd = debug::Queue(debug::Action::pose_dump);
    const std::uint32_t sps = debug::Queue(debug::Action::pose_scan);
    const std::uint32_t sla = debug::Queue(debug::Action::look_at_body);
    debug::Drain();
    CHECK(debug::ResultFor(scen, &r) && r.ok && debug::ResultFor(spd, &r) && r.ok && debug::ResultFor(sps, &r) && r.ok &&
          debug::ResultFor(sla, &r) && r.ok && game::stub::censuses.load() == 4);
    const std::uint32_t srp = debug::Queue(debug::Action::real_probe);
    const std::uint32_t srm = debug::Queue(debug::Action::real_mirror);
    debug::Drain();
    CHECK(debug::ResultFor(srp, &r) && r.ok && debug::ResultFor(srm, &r) && r.ok && game::stub::real_cmds.load() == 2 &&
          game::stub::last_real_op == 6);

    debug::Request e;
    e.action = debug::Action::set_echo_tuning;
    e.f[0] = 3.5f;
    e.f[1] = -1.25f;
    debug::Queue(e);
    debug::Request v;
    v.action = debug::Action::set_verbose;
    v.flag = true;
    debug::Queue(v);
    debug::Queue(debug::Action::hide_partner);
    debug::Queue(debug::Action::remove_partner);
    debug::Queue(debug::Action::log_positions);
    debug::Queue(debug::Action::log_partner);
    debug::Queue(debug::Action::log_status);
    CHECK(debug::Drain() == 7);
    CHECK(std::fabs(sync::stub::delay - 3.5f) < 1e-6f && std::fabs(sync::stub::offset + 1.25f) < 1e-6f);
    CHECK(sync::stub::verbose);
    CHECK(sync::stub::log_positions.load() >= 1 && sync::stub::log_partner.load() >= 1 && sync::stub::removes.load() >= 1);
    CHECK(LogHas("[debug] status: role="));

    // A test handler replaces the game call.
    debug::SetHandlerForTest(debug::Action::show_partner, [](const debug::Request&, char* msg, std::size_t cap) {
        std::snprintf(msg, cap, "fake show");
        return true;
    });
    const std::uint32_t s4 = debug::Queue(debug::Action::show_partner);
    debug::Drain();
    CHECK(debug::ResultFor(s4, &r) && r.ok && std::strcmp(r.text, "fake show") == 0);
    debug::ResetForTest();
}

void TestSnapshot() {
    debug::ResetForTest();
    debug::SetRoleLabel("echo");
    debug::SetModState("ready");
    const std::uint64_t before = debug::GetSnapshot().serial;
    debug::SetMenuOpen(true);
    CHECK(debug::MenuOpen());
    debug::OnTick(1000.0);
    debug::OnTick(1001.0);  // menu open: publishes every tick
    debug::Snapshot s = debug::GetSnapshot();
    CHECK(s.serial == before + 2);
    CHECK(std::strcmp(s.role, "echo") == 0);
    CHECK(std::strcmp(s.mod_state, "ready") == 0);
    CHECK(s.local_valid && std::fabs(s.local_pos[0] - 1000.0) < 1e-9);
    CHECK(std::strcmp(s.link_state, "connected") == 0);
    CHECK(s.diag.resolved && s.diag.health_valid && s.diag.health == 85.0f && s.diag.area == 200);
    CHECK(game::stub::ticks.load() >= 2);  // OnTick runs the game-side tick too
    debug::SetMenuOpen(false);
    debug::OnTick(1002.0);  // closed: at most every 500 ms
    CHECK(debug::GetSnapshot().serial == before + 2);
    debug::OnTick(1502.0);
    CHECK(debug::GetSnapshot().serial == before + 3);
    // Actions queued from another thread run inside OnTick.
    const int r0 = sync::stub::respawns.load();
    std::thread([] { debug::Queue(debug::Action::respawn_partner); }).join();
    debug::OnTick(1503.0);
    CHECK(sync::stub::respawns.load() == r0 + 1);
}

void TestDecoders() {
    char b[256];
    CHECK(std::strcmp(game::GameStateFlagName(0), "TerminalOperation") == 0);
    CHECK(std::strcmp(game::GameStateFlagName(6), "Cutscene") == 0);
    CHECK(std::strcmp(game::GameStateFlagName(22), "Loading") == 0);
    CHECK(std::strcmp(game::GameStateFlagName(26), "Setup") == 0);
    CHECK(std::strcmp(game::GameStateFlagName(30), "DirectIntoPrivateRoom") == 0);
    CHECK(game::GameStateFlagName(31) == nullptr && game::GameStateFlagName(-1) == nullptr);
    static_assert(game::kGameStateSetup == (1ull << 26), "Setup bit");
    game::DescribeGameStateFlags(0, b, sizeof(b));
    CHECK(std::strcmp(b, "none") == 0);
    game::DescribeGameStateFlags(0x40 | 0x400000 | (1ull << 40), b, sizeof(b));
    CHECK(std::strcmp(b, "Cutscene|Loading|bit40") == 0);
    char narrow[12];  // truncation never overflows
    game::DescribeGameStateFlags(~0ull, narrow, sizeof(narrow));
    CHECK(std::strlen(narrow) < sizeof(narrow));
    // The stopping mask covers the pause menu, cutscenes, knot space, fast travel, game over, faint, loading.
    for (int bit : {2, 6, 9, 12, 13, 14, 22}) CHECK((game::kStoppingGameEventMask & (1ull << bit)) != 0);
    static_assert((game::kStoppingGameEventMask & (1ull << 15)) == 0, "BossBattle does not stop");
    game::DescribeLoadMask(0, b, sizeof(b));
    CHECK(std::strcmp(b, "none") == 0);
    game::DescribeLoadMask(0x102 | 0x1000, b, sizeof(b));
    CHECK(std::strcmp(b, "loading/area change|scene wait|0x1000") == 0);  // 0x100: a sequence waits for its scene, not a save
    CHECK(std::strcmp(game::AreaName(100), "Area01") == 0 && std::strcmp(game::AreaName(0xFFFF), "Empty") == 0);
    CHECK(game::AreaName(123) == nullptr);
    CHECK(game::TeleportTargetSane(1000, -2000, 300));
    CHECK(!game::TeleportTargetSane(std::nan(""), 0, 0));
    CHECK(!game::TeleportTargetSane(0, HUGE_VAL, 0));
    CHECK(!game::TeleportTargetSane(1e6, 0, 0));
    game::FormatClock(14.5f, b, sizeof(b));
    CHECK(std::strcmp(b, "14:30") == 0);
    game::FormatClock(23.999f, b, sizeof(b));
    CHECK(std::strcmp(b, "00:00") == 0);
    game::FormatClock(-1.0f, b, sizeof(b));
    CHECK(std::strcmp(b, "?") == 0);
}

void TestDiagnosticsReport() {
    debug::Snapshot s;
    char text[4096];
    std::size_t n = debug::FormatDiagnostics(s, text, sizeof(text));
    CHECK(n == std::strlen(text) && std::strstr(text, "game: not read") != nullptr);
    sync::FillDebugSnapshot(s);
    game::ReadDiagnostics(&s.diag);
    s.diag.state_flags = 0x40;
    s.diag.teleport_ready = false;
    std::snprintf(s.diag.teleport_block, sizeof(s.diag.teleport_block), "game state: Cutscene");
    n = debug::FormatDiagnostics(s, text, sizeof(text));
    CHECK(n == std::strlen(text));
    CHECK(std::strstr(text, "game state: 0x40 [Cutscene]") != nullptr);
    CHECK(std::strstr(text, "area: 200 Area02") != nullptr);
    CHECK(std::strstr(text, "time of day: 14:30") != nullptr);
    CHECK(std::strstr(text, "health: 85.0 / 100.0") != nullptr);
    CHECK(std::strstr(text, "teleport: blocked: game state: Cutscene") != nullptr);
    CHECK(std::strstr(text, "voice ping: DSEchoComponent::Play") != nullptr);
    CHECK(std::strstr(text, "partner=") != nullptr);
    char tiny[40];  // a small buffer is filled and terminated, never overrun
    std::memset(tiny, 'x', sizeof(tiny));
    n = debug::FormatDiagnostics(s, tiny, sizeof(tiny));
    CHECK(n < sizeof(tiny) && tiny[n] == 0);
}

void TestLogRing() {
    const std::uint64_t start = LogNewestSeq();
    for (int i = 0; i < 600; ++i) Log("ring line %d", i);
    CHECK(LogNewestSeq() == start + 600);
    std::vector<LogLine> buf(1000);
    std::size_t n = LogCopyRecent(0, buf.data(), buf.size());
    CHECK(n == kLogRingLines);
    for (std::size_t i = 1; i < n; ++i) CHECK(buf[i].seq == buf[i - 1].seq + 1);
    CHECK(std::strstr(buf[n - 1].text, "ring line 599") != nullptr);
    CHECK(buf[n - 1].text[std::strlen(buf[n - 1].text) - 1] != '\n');
    // after_seq: only newer lines
    n = LogCopyRecent(start + 595, buf.data(), buf.size());
    CHECK(n == 5);
    CHECK(std::strstr(buf[0].text, "ring line 595") != nullptr);
    // max keeps the newest
    n = LogCopyRecent(0, buf.data(), 3);
    CHECK(n == 3 && std::strstr(buf[2].text, "ring line 599") && std::strstr(buf[0].text, "ring line 597"));
    // long lines are truncated, not overflowed
    std::string longline(3000, 'x');
    Log("%s", longline.c_str());
    n = LogCopyRecent(LogNewestSeq() - 1, buf.data(), 1);
    CHECK(n == 1 && std::strlen(buf[0].text) == sizeof(buf[0].text) - 1);
    CHECK(LogCopyRecent(LogNewestSeq(), buf.data(), 10) == 0);
}

void TestPositions() {
    using namespace st::overlay;
    const std::string text =
        "# comment\r\n"
        "1.5 2.5 -3.5 Knot City gate\r\n"
        "\n"
        "  (10, 20, 30)   Lake  \n"
        "bad line\n"
        "1 2\n"
        "4;5;6\n"
        "7 8 9abc nope\n"
        "1e3 -2e2 0.001";
    const auto list = ParsePositions(text);
    CHECK(list.size() == 4);
    if (list.size() == 4) {
        CHECK(list[0].name == "Knot City gate" && list[0].x == 1.5 && list[0].z == -3.5);
        CHECK(list[1].name == "Lake" && list[1].y == 20);
        CHECK(list[2].name == "position 3" && list[2].x == 4 && list[2].z == 6);
        CHECK(list[3].x == 1000 && list[3].y == -200 && list[3].name == "position 4");
    }
    const auto again = ParsePositions(FormatPositions(list));
    CHECK(again.size() == list.size());
    for (std::size_t i = 0; i < again.size() && i < list.size(); ++i) {
        CHECK(again[i].name == list[i].name);
        CHECK(std::fabs(again[i].x - list[i].x) < 1e-3 && std::fabs(again[i].y - list[i].y) < 1e-3 &&
              std::fabs(again[i].z - list[i].z) < 1e-3);
    }
    double v[3];
    CHECK(ParseCoordinates("1 2 3", v) && v[2] == 3);
    CHECK(ParseCoordinates("(1.5, -2.25, 3e2)", v) && v[0] == 1.5 && v[1] == -2.25 && v[2] == 300);
    CHECK(ParseCoordinates(FormatCoordinates(12.3456, -7.0, 0.5).c_str(), v) && std::fabs(v[0] - 12.346) < 1e-9);
    CHECK(!ParseCoordinates("1 2", v));
    CHECK(!ParseCoordinates("1 2 3 4", v));
    CHECK(!ParseCoordinates("a b c", v));
    CHECK(!ParseCoordinates(nullptr, v));
    CHECK(!ParseCoordinates("nan 1 2", v));
    CHECK(CleanName("  a\tb\nc  ", "x") == "abc");
    CHECK(CleanName("   ", "fallback") == "fallback");
    CHECK(CleanName(std::string(100, 'n'), "x").size() == 64);

    // File round trip in %TEMP%.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"st_overlay_tests_positions.txt";
    CHECK(SavePositions(path, list));
    std::vector<SavedPosition> loaded;
    CHECK(LoadPositions(path, loaded) && loaded.size() == list.size());
    DeleteFileW(path.c_str());
    CHECK(!LoadPositions(path, loaded) && loaded.empty());
}

void TestKeys() {
    using namespace st::overlay;
    char b[24];
    CHECK(std::strcmp(VkName(0x78, b, sizeof(b)), "F9") == 0);
    CHECK(std::strcmp(VkName(0x7B, b, sizeof(b)), "F12") == 0);
    CHECK(std::strcmp(VkName(0x2D, b, sizeof(b)), "Insert") == 0);
    CHECK(std::strcmp(VkName('K', b, sizeof(b)), "K") == 0);
    CHECK(std::strcmp(VkName(0xE2, b, sizeof(b)), "VK 0xE2") == 0);
    CHECK(VkToImGuiKey('A', false) == ImGuiKey_A);
    CHECK(VkToImGuiKey('9', false) == ImGuiKey_9);
    CHECK(VkToImGuiKey(0x78, false) == ImGuiKey_F9);
    CHECK(VkToImGuiKey(0x0D, false) == ImGuiKey_Enter);
    CHECK(VkToImGuiKey(0x0D, true) == ImGuiKey_KeypadEnter);
    CHECK(VkToImGuiKey(0x11, false) == ImGuiKey_LeftCtrl);
    CHECK(VkToImGuiKey(0x08, false) == ImGuiKey_Backspace);
    CHECK(VkToImGuiKey(0xFF, false) == ImGuiKey_None);
}

// The mouse lock's decision (mouse_lock.hpp), rule by rule. Pure: nothing here touches the real cursor.
void TestMouseLock() {
    using namespace st::overlay::mouselock;
    State s;
    s.enabled = true;
    s.foreground = true;
    s.engaged = true;
    s.client = RECT{100, 50, 1380, 770};  // 1280 x 720 at (100, 50)
    // 6: no clip in effect -> clip to the client area
    Decision d = Decide(s);
    CHECK(d.act == Act::clip && d.why == Why::clipping && SameRect(d.rect, s.client));
    // 5: our clip (== the client area) held -> keep
    s.have_clip = true;
    s.clip = s.client;
    s.ours = true;
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::held);
    // 5: the game's one-pixel centre clip inside the window (gameplay) -> keep, it is tighter
    s.ours = false;
    s.clip = RECT{739, 409, 740, 410};
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::game_clip);
    // 6: a clip outside / bigger than the window (the game's window-rect clip with the title bar, a foreign one) -> clip
    s.clip = RECT{92, 19, 1388, 778};
    d = Decide(s);
    CHECK(d.act == Act::clip && SameRect(d.rect, s.client));
    // 3: not in front -> release ONLY our own clip, never a clip someone else set
    s.foreground = false;
    s.clip = s.client;
    s.ours = true;
    d = Decide(s);
    CHECK(d.act == Act::release && d.why == Why::not_front);
    s.ours = false;
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::not_front);
    s.have_clip = false;
    d = Decide(s);
    CHECK(d.act == Act::keep);
    // 3: minimised, moving, an empty (zero-size) client area -> released when ours
    s.foreground = true;
    s.have_clip = true;
    s.ours = true;
    s.iconic = true;
    d = Decide(s);
    CHECK(d.act == Act::release && d.why == Why::minimised);
    s.iconic = false;
    s.size_move = true;
    d = Decide(s);
    CHECK(d.act == Act::release && d.why == Why::moving);
    s.size_move = false;
    const RECT keep_client = s.client;
    s.client = RECT{-32000, -32000, -32000, -32000};
    d = Decide(s);
    CHECK(d.act == Act::release && d.why == Why::no_client);
    s.client = keep_client;
    // 4: in front but not engaged (activated by the taskbar / Alt+Tab with the cursor elsewhere) -> keep
    s.engaged = false;
    s.have_clip = false;
    s.ours = false;
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::not_engaged);
    s.engaged = true;
    // 1: off -> release only ours
    s.enabled = false;
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::off);
    s.have_clip = true;
    s.clip = s.client;
    s.ours = true;
    d = Decide(s);
    CHECK(d.act == Act::release && d.why == Why::off);
    s.enabled = true;
    // 2: a hidden run never acts, whatever the state
    s.automation = true;
    s.have_clip = false;
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::automation);
    s.foreground = false;
    s.ours = true;
    s.have_clip = true;
    d = Decide(s);
    CHECK(d.act == Act::keep && d.why == Why::automation);
    s.automation = false;
    s.foreground = true;
    // multi-monitor: a window on a monitor left of the primary (negative coordinates)
    State m;
    m.enabled = m.foreground = m.engaged = true;
    m.client = RECT{-1900, -200, -620, 520};
    d = Decide(m);
    CHECK(d.act == Act::clip && SameRect(d.rect, m.client));
    m.have_clip = true;
    m.clip = RECT{-1261, 159, -1260, 160};  // the game's centre pixel there
    CHECK(Decide(m).why == Why::game_clip);
    m.clip = RECT{0, 0, 1, 1};  // a pixel on the primary monitor: not inside this window
    CHECK(Decide(m).act == Act::clip);
    // the game's ClipCursor(NULL) is replaced only while the lock holds
    CHECK(ReplaceGameRelease(s));
    State r = s;
    r.engaged = false;
    CHECK(!ReplaceGameRelease(r));
    r = s;
    r.foreground = false;
    CHECK(!ReplaceGameRelease(r));
    r = s;
    r.automation = true;
    CHECK(!ReplaceGameRelease(r));
    r = s;
    r.enabled = false;
    CHECK(!ReplaceGameRelease(r));
    r = s;
    r.size_move = true;
    CHECK(!ReplaceGameRelease(r));
    // the switch key: F10, F11 when the menu key is F10
    CHECK(SwitchKey(VK_F9) == VK_F10 && SwitchKey(VK_F10) == VK_F11 && SwitchKey(VK_F11) == VK_F10);
    for (int w = 0; w <= static_cast<int>(Why::clipping); ++w) CHECK(WhyText(static_cast<Why>(w))[0] != '?');
}

}  // namespace

int main() {
    TestRegistry();
    TestQueueThreads();
    TestQueueFull();
    TestActions();
    TestSnapshot();
    TestDecoders();
    TestDiagnosticsReport();
    TestLogRing();
    TestPositions();
    TestKeys();
    TestMouseLock();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
