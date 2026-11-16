#pragma once
// Stranded Together launcher: the model behind the GUI shell. Adapted from
// Diamond Dogs Together's LauncherModel, trimmed for milestone 1: no setup
// steps, characters, HUD, saves or local two-window test; added the install
// checks, the transport (Steam or LAN/UDP) and the solo echo test. The update
// check and self-update from GitHub Releases are DDT's (core/core_update.hpp).
//
// Every string is UTF-8 and player-facing. Threading: the GUI calls every
// member from its one UI thread; tick() runs every frame (at least once a
// second, about 4 Hz while a game runs).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace st {

// solo: the debug tools only (in-game debug menu, logging), no partner and no network.
enum class Role { host, join, echo, solo };
enum class Transport { steam, udp };
// not_in_game_folder: no ds.exe next to the launcher (a full-window message).
// error: co-op cannot start here (the mod is missing, a folder problem).
enum class Phase { not_in_game_folder, error, ready, launching, in_game, after_game };

// What winhttp.dll in the game folder is.
//  ok         its SHA-256 is the one stranded_together\package.json names (a release install)
//  dev_build  ours (the Stranded Together proxy), but there is no package.json (a developer deploy)
//  mismatch   ours, but not the package's file (or the package is damaged / of another version): a warning
//  foreign    another program's winhttp.dll (another mod, a loader): never touched, co-op can't start
//  disabled   only winhttp.dll.disabled (ours) is there: Debug > Enable mod brings it back
//  missing    neither
enum class ModState { missing, ok, dev_build, mismatch, foreign, disabled };

// The install gate as the Play page shows it.
struct InstallChecks {
    bool game_found = false;       // ds.exe next to the launcher
    bool version_tested = true;    // ds.exe / steam_api64.dll are the tested build (false: warning only)
    std::string version_note;      // the warning sentence when not
    bool mod_present = false;      // winhttp.dll in the game folder is ours (ok, dev_build or mismatch)
    ModState mod_state = ModState::missing;
    std::string mod_note;          // mismatch / foreign / disabled: the sentence for the player
    std::string package_version;   // package.json "version" ("" without one)
    bool steam_running = false;    // Steam runs with a logged-in user
};

// The Debug page's options. Saved in launcher-settings.json ("debug") and written
// into every session.ini (Host, Join, Solo test, Debug tools only).
struct DebugOptions {
    double echo_delay_s = 2.0;     // echo: the clone replays your movement this far behind (0..10)
    double echo_offset_m = 1.5;    // echo: sideways offset (0..20)
    int send_hz = 30;              // pose packets per second (1..120)
    int interp_delay_ms = 100;     // partner interpolation delay (0..1000)
    bool partner_activate = true;  // wake the spawned partner clone (0 = leave it asleep)
    bool verbose = false;          // verbose mod log
    bool debug_menu = true;        // the in-game debug menu
    int debug_menu_key = 9;        // its key: F1..F12 (the number)
};

// The Advanced / LAN section (transport udp).
struct LanSettings {
    std::string peer_ip;           // the partner's IPv4
    int port_local = 27555;
    int port_peer = 27555;
};

struct LinkView {
    std::string state;             // the mod's state ("" before the first status)
    bool fresh = false;            // the status is recent and its game runs
    bool connected = false;
    int ping_ms = 0;               // <= 0: unknown
    std::string transport;         // steam | udp
    std::string route;             // relay | direct | ""
    std::string peer;              // the partner as the mod names them
    std::string remote;            // none | spawning | visible | failed
    std::string headline;          // one short line, e.g. "Connected to Sam"
    std::string detail;            // plain-language sentence(s)
    enum class Tone { neutral, good, warn, bad } tone = Tone::neutral;
};

// The launcher's update from GitHub Releases (core/core_update.hpp).
//  idle             nothing checked yet in this window
//  checking         the release feed is being read
//  up_to_date       no newer release (or none with a StrandedTogether-<version>.zip)
//  available        latest_version is newer: Update now can be pressed
//  downloading      the zip is downloading (progress)
//  applying         the zip is verified and installed, then the new launcher starts
//  ready_to_restart the new launcher runs: this window closes
//  error            the check or the update failed; `error` says why (Update now / Check again retry)
enum class UpdateStatus { idle, checking, up_to_date, available, downloading, applying, ready_to_restart, error };

struct UpdateInfo {
    UpdateStatus status = UpdateStatus::idle;
    std::string current_version;      // this launcher ("0.1.0+abc1234")
    std::string latest_version;       // "0.2.0" (the tag without its v)
    std::string tag;                  // "v0.2.0"
    std::string release_title;
    std::string release_notes;        // the release body (Markdown as written)
    std::string html_url;             // the release page
    std::string asset_name;           // StrandedTogether-0.2.0.zip
    std::string download_url;
    std::string asset_sha256;         // upper case, from the feed's asset digest ("" when GitHub gave none)
    std::uint64_t download_size = 0;
    std::uint64_t bytes_downloaded = 0;
    float progress = 0.0f;            // 0..1 while downloading
    std::string error;                // status error: a sentence for the player
    bool error_after_check = false;   // the error came from a check (no release is known), not from Update now
    std::string updated_to;           // right after a self-update: the version now running ("" otherwise)
    bool auto_check = true;           // the setting: check once when the window opens
};

struct CodeCheck {
    bool ok = false;
    std::string message;           // "" for an empty field; otherwise a sentence for the player
};

class LauncherModel {
public:
    virtual ~LauncherModel() = default;
    virtual Phase phase() const = 0;
    virtual std::string version() const = 0;
    virtual std::string game_dir() const = 0;
    // not_in_game_folder: the DEATH STRANDING folder Steam knows about, "" when unknown.
    virtual std::string discovered_game_dir() const = 0;
    virtual InstallChecks checks() const = 0;
    virtual std::string last_error() const = 0;
    // error / not_in_game_folder: run the start-up checks again.
    virtual void recheck() = 0;

    // Persisted choices (launcher-settings.json).
    virtual Transport transport() const = 0;
    virtual void set_transport(Transport transport) = 0;
    virtual LanSettings lan() const = 0;
    virtual void set_lan(const LanSettings& lan) = 0;
    virtual std::string player_name() const = 0;   // "" = the mod picks (Steam name)
    virtual void set_player_name(const std::string& name) = 0;

    // Sessions: each writes session.ini and starts the game through Steam.
    virtual bool host(std::string& code_out, std::string& error_out) = 0;
    virtual CodeCheck check_code(const std::string& code) const = 0;
    virtual bool join(const std::string& code, std::string& error_out) = 0;
    virtual bool solo_test(std::string& error_out) = 0;
    // Debug > Debug tools only: role=solo (the in-game debug menu and logging, no partner, no network).
    virtual bool debug_solo(std::string& error_out) = 0;
    // launching, before the game process exists: undo the session and go back to ready.
    virtual bool cancel_launch(std::string& error_out) = 0;
    // No game running: remove a leftover session (Tools > End session).
    virtual bool end_session(std::string& error_out) = 0;

    // While a session runs (launching, in_game, after_game).
    virtual Role session_role() const = 0;
    virtual Transport session_transport() const = 0;
    virtual std::string join_code() const = 0;     // the host's code ("" otherwise). Never logged.
    virtual LinkView link() const = 0;
    virtual bool session_pending() const = 0;      // a session record or session.ini exists (End session can do something)

    // Tools.
    virtual std::string mod_dir() const = 0;       // <game>\stranded_together
    virtual std::string mod_log_path() const = 0;  // <game>\stranded_together\stranded_together.log
    virtual bool collect_logs(std::string& path_out, std::string& error_out) = 0;

    // Debug page.
    virtual DebugOptions debug_options() const = 0;
    virtual void set_debug_options(const DebugOptions& options) = 0;
    // The last `max_bytes` of the mod's log ("" when it does not exist yet).
    virtual std::string mod_log_tail(std::size_t max_bytes) const = 0;
    // The game's crash report (<game>\error.txt); false when there is none.
    virtual bool has_crash_report() const = 0;
    virtual bool crash_report(std::string& text_out) const = 0;
    // Renames winhttp.dll <-> winhttp.dll.disabled, only when it is ours and the game is not running.
    virtual bool set_mod_enabled(bool enabled, std::string& error_out) = 0;
    // Removes session.ini (also one this launcher did not write) and ends the recorded session.
    // Not while the game runs.
    virtual bool clear_session(std::string& error_out) = 0;

    // Updates (GitHub Releases). check_for_updates: Check for updates (a no-op while one runs);
    // start_update: Update now (download, verify, install, restart), refused with a sentence in
    // update_info().error while the game runs or a session is active.
    virtual UpdateInfo update_info() const = 0;
    virtual void check_for_updates() = 0;
    virtual void start_update() = 0;
    virtual void set_auto_update_check(bool on) = 0;   // persisted ("auto_update_check")

    virtual void tick() = 0;
};

// How the window was started (launcher_main.cpp).
struct ModelStart {
    bool check_on_start = false;      // check for updates once (when the setting allows): the real window only
    std::uint32_t after_update = 0;   // --after-update <pid>: this exe replaced that launcher
    std::string update_feed;          // --update-feed <url> (developer): this release feed instead of GitHub's
    bool update_now = false;          // --update-now (developer, with --update-feed): install a found update at once
};

std::unique_ptr<LauncherModel> MakeRealModel(const std::wstring& exe_dir, const ModelStart& start);

}  // namespace st
