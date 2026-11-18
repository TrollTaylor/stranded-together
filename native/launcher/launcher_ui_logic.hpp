#pragma once
// Stranded Together: the pure part of the GUI shell (no ImGui, no Win32), so
// it can be unit-tested headless (tests/launcher_core_tests.cpp): page
// navigation, the Play tabs, which buttons can be pressed, the Join field's
// message/tone, join-code display, redraw pacing, the update banner, the
// screenshot scenes and the command line. Adapted from Diamond Dogs Together's
// launcher_ui_logic.
#include <cstdint>
#include <string>
#include <vector>

#include "launcher_model.hpp"

namespace st::ui {

enum class Tone { neutral, good, warn, bad };

// ---- Pages and navigation --------------------------------------------------
// "Not in the game folder" is a full-window message instead of a page.
enum class Page { play, in_game, tools, debug, about };
inline constexpr int kPageCount = 5;
const char* PageLabel(Page page);

// A game session exists (the game is starting, running or just closed).
bool SessionActive(Phase phase);
// Host, Join and Solo test can be pressed at all (co-op is ready and no game runs).
bool CanStartSession(Phase phase);
// Host needs Steam (the code carries the host's SteamID64).
bool CanHost(Phase phase, const InstallChecks& checks);
bool PageVisible(Phase phase, Page page);
bool PageEnabled(Phase phase, Page page);
Page DefaultPage(Phase phase);
// Automatic navigation when the model's phase changes: play -> in game -> play,
// leaving the player on Tools or About.
Page PageAfterPhaseChange(Phase from, Phase to, Page current);

// ---- Play tabs -------------------------------------------------------------
enum class PlayTab { host, join, solo };
inline constexpr int kPlayTabCount = 3;
const char* PlayTabLabel(PlayTab tab);   // "Host" | "Join" | "Solo test"
PlayTab TabForRole(Role role);

// ---- Debug page --------------------------------------------------------------
enum class DebugTab { options, saves, files };
inline constexpr int kDebugTabCount = 3;
const char* DebugTabLabel(DebugTab tab);   // "Options" | "Test saves" | "Files and log"
// The F-key list of the debug-menu key combo: "F1\0F2\0...F12\0\0" (ImGui::Combo items).
const char* FunctionKeyItems();
// The chip / headline for the mod state ("Mod 0.1.0", "Mod: development build", ...).
std::string ModChipText(const InstallChecks& checks);
Tone ModTone(ModState state);
// The last `max_lines` lines of a log text (the live tail on the Debug page).
std::string LastLines(const std::string& text, std::size_t max_lines);

// ---- Join field ------------------------------------------------------------
struct JoinFieldView {
    Tone tone = Tone::neutral;
    std::string message;
    bool can_join = false;
    bool field_enabled = true;
};
// What the Join tab shows under the code field and whether "Join game" is
// enabled. `check` is the model's check_code() of `text`; `busy` = a session runs.
JoinFieldView EvaluateJoinField(const std::string& text, const CodeCheck& check, bool busy);

// ---- LAN settings ------------------------------------------------------------
// "" when the Advanced / LAN fields can start a UDP session, else a sentence.
std::string LanProblem(const LanSettings& lan);
// Parses a port field: 1..65535, digits only.
bool ParsePort(const std::string& text, int& out);

// ---- Join code display -----------------------------------------------------
// "DST1-AAAAA-BBBBB-CCCCC-DDDDD-CC@76561197960265729" -> main
// "DST1-AAAAA-BBBBB-CCCCC-DDDDD-CC", account "@76561197960265729".
struct CodeDisplay {
    std::string main;
    std::string account;
};
CodeDisplay SplitJoinCode(const std::string& code);

// ---- Link status -----------------------------------------------------------
Tone PingTone(int ms);                  // <= 0 neutral, < 90 good, < 180 warn, else bad
std::string FormatPing(int ms);         // "42 ms", or "-" when unknown
std::string RouteText(const std::string& transport, const std::string& route);   // "Steam relay", "LAN (direct)"
std::string RemoteText(const std::string& remote);   // "Visible", "Appearing...", ...

// ---- Redraw pacing ---------------------------------------------------------
int IdleWaitMs(Phase phase, bool animating);

// ---- Updates -----------------------------------------------------------------
// The card on top of Play: an update to install, its download, its install, Update
// now that did not go through (its sentence, with Update now again), or the note
// right after a self-update. A failed or up-to-date check shows on About only.
enum class UpdateBanner { none, available, downloading, installing, failed, updated };
UpdateBanner UpdateBannerFor(const UpdateInfo& info);
// Update now can be pressed: a release is known, no game or session runs from here.
bool CanStartUpdate(Phase phase, const UpdateInfo& info);
// Check for updates can be pressed (nothing is checking, downloading or installing).
bool CanCheckForUpdates(const UpdateInfo& info);
// A download, install or handover is running (the window keeps redrawing).
bool UpdateInProgress(const UpdateInfo& info);
// The window closes itself: the updated launcher has taken over.
bool CloseForUpdate(const UpdateInfo& info);
// The About page's one line on updates ("You have the latest version.", ...).
std::string UpdateStatusText(const UpdateInfo& info);
// The first lines of the release notes as plain text: Markdown headings and
// emphasis markers dropped, bullets as "- ", blank lines collapsed, at most
// max_lines lines and max_chars characters ("..." when cut). "" for none.
std::string NotesExcerpt(const std::string& notes, std::size_t max_lines, std::size_t max_chars);
// "1.8 MB", "640 KB", "512 bytes".
std::string FormatBytes(std::uint64_t bytes);

// ---- Screenshot scenes (--screenshot-all) ------------------------------------
enum class Scene {
    play_host,
    play_host_launching,
    play_join,
    play_join_invalid,
    play_join_valid,
    play_solo,
    play_lan,
    play_untested_version,
    play_steam_offline,
    play_dev_build,
    play_mod_mismatch,
    not_installed,
    mod_foreign,
    mod_disabled,
    wrong_folder,
    ingame_launching,
    ingame_host_waiting,
    ingame_connected,
    ingame_echo,
    ingame_solo,
    ingame_not_responding,
    ingame_lost,
    ingame_after_game,
    tools,
    tools_result,
    tools_in_game,
    debug_options,
    debug_files,
    debug_in_game,
    about,
    play_update_available,
    play_update_downloading,
    play_update_failed,
    play_updated,
    about_updates,
    debug_test_saves,      // Debug > Test saves: a test save picked, the local test idle
    debug_local_test,      // Debug > Test saves: the local two-player test running, both games linked
    play_join_characters,  // Join: the character list open
    play_join_hologram,    // Join with Debug > Partner body = Hologram: the "you are sent as a hologram" note
    debug_options_characters,  // Debug > Options at PARTNER BODY: "Allow other characters" on
    count
};
int SceneCount();
const char* SceneName(Scene scene);   // "" when out of range

// ---- Command line ----------------------------------------------------------
struct Options {
    enum class Mode { gui, watch, selftest, screenshots, help, write_session, end_session } mode = Mode::gui;
    bool mock = false;              // --mock (developer flag; the real model is the default)
    std::wstring watch_session;     // --watch <session>
    // Developer: --write-session host|join|echo|solo [--code <code>] [--game-dir <dir>]
    //            [--transport steam|udp] [--udp-peer <ip>] [--udp-port-local <n>] [--udp-port-peer <n>]
    std::wstring write_role;        // host | join | echo | solo
    std::wstring write_code;
    std::wstring write_transport;
    std::wstring write_udp_peer;
    int write_port_local = 0;
    int write_port_peer = 0;
    std::wstring game_dir;          // --game-dir <dir> (default: the exe's folder)
    std::wstring screenshot_dir;    // --screenshot-all <dir>
    std::wstring scenario;          // --mock-scenario <name>
    double mock_speed = 1.0;        // --mock-speed <factor>
    float scale = 0.0f;             // --scale <factor>; 0 = the monitor's DPI
    // The restart after a self-update (the old launcher passes it, core_update.hpp):
    std::uint32_t after_update = 0; // --after-update <pid>: wait for that launcher to close first
    // Developer: --update-feed <url>, a release feed on this PC (http://127.0.0.1:<port>/...) instead of GitHub's.
    std::wstring update_feed;
    bool update_now = false;        // --update-now: install an update found on that feed at once (needs --update-feed)
    std::wstring error;             // non-empty: the command line was refused
};
// args excludes the program name.
Options ParseCommandLine(const std::vector<std::wstring>& args);
const wchar_t* UsageText();

}  // namespace st::ui
