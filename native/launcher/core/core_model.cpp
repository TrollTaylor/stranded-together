// Stranded Together launcher core: the real LauncherModel (see core_model.hpp).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core_model.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#include "core_install.hpp"
#include "core_log.hpp"
#include "core_paths.hpp"
#include "core_sandbox.hpp"
#include "core_session.hpp"
#include "core_text.hpp"
#include "core_update.hpp"
#include "launcher_join_code.hpp"
#include "sandbox.hpp"   // native/mod: kLocalTestFps, the game's frame limit range

#ifndef ST_LAUNCHER_VERSION
#define ST_LAUNCHER_VERSION "0.0.0-dev"
#endif

namespace st::core {
namespace {

constexpr std::int64_t kCheckEveryMs = 2000;
constexpr std::int64_t kStatusEveryMs = 500;
constexpr std::int64_t kExitConfirmMs = 4000;     // the game gone this long counts as exited
constexpr std::int64_t kAfterGameMs = 4000;

const char* TransportName(Transport t) { return t == Transport::udp ? "udp" : "steam"; }

LinkView::Tone ToneOf(StatusView::Tone t) {
    switch (t) {
    case StatusView::Tone::good: return LinkView::Tone::good;
    case StatusView::Tone::warn: return LinkView::Tone::warn;
    case StatusView::Tone::bad: return LinkView::Tone::bad;
    case StatusView::Tone::neutral: break;
    }
    return LinkView::Tone::neutral;
}

std::string RoleIniName(Role r) {
    switch (r) {
    case Role::host: return "host";
    case Role::join: return "guest";
    case Role::echo: return "echo";
    case Role::solo: return "solo";
    }
    return "echo";
}

Role RoleFromIni(const std::string& role) {
    return role == "guest" ? Role::join : role == "echo" ? Role::echo : role == "solo" ? Role::solo : Role::host;
}

const char* ModStateName(ModState s) {
    switch (s) {
    case ModState::missing: return "missing";
    case ModState::ok: return "ok";
    case ModState::dev_build: return "dev_build";
    case ModState::mismatch: return "mismatch";
    case ModState::foreign: return "foreign";
    case ModState::disabled: return "disabled";
    }
    return "?";
}

// Size and last write of a file ("-" when absent): a change re-runs the mod check.
std::string FileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return "-";
    return std::to_string(data.nFileSizeHigh) + ":" + std::to_string(data.nFileSizeLow) + ":" + std::to_string(data.ftLastWriteTime.dwHighDateTime) +
           ":" + std::to_string(data.ftLastWriteTime.dwLowDateTime);
}

}  // namespace

DebugOptions ClampDebugOptions(DebugOptions o) {
    const DebugOptions d;
    if (!(o.echo_delay_s >= 0.0 && o.echo_delay_s <= kMaxEchoDelayS)) o.echo_delay_s = d.echo_delay_s;
    if (!(o.echo_offset_m >= 0.0 && o.echo_offset_m <= kMaxEchoOffsetM)) o.echo_offset_m = d.echo_offset_m;
    // Two decimals at most: what session.ini carries.
    o.echo_delay_s = static_cast<double>(static_cast<long long>(o.echo_delay_s * 100.0 + 0.5)) / 100.0;
    o.echo_offset_m = static_cast<double>(static_cast<long long>(o.echo_offset_m * 100.0 + 0.5)) / 100.0;
    if (o.send_hz < 1 || o.send_hz > 120) o.send_hz = d.send_hz;
    if (o.interp_delay_ms < 0 || o.interp_delay_ms > 1000) o.interp_delay_ms = d.interp_delay_ms;
    if (o.partner_volume < 0 || o.partner_volume > 100) o.partner_volume = d.partner_volume;
    if (o.debug_menu_key < 1 || o.debug_menu_key > 12) o.debug_menu_key = d.debug_menu_key;
    return o;
}

DebugOptions DebugOptionsFromSettings(const json::Value& settings) {
    DebugOptions o;
    const json::Value* d = settings.Find("debug");
    if (!d || !d->is_object()) return o;
    auto num = [&](const char* key, double fallback) {
        const json::Value* v = d->Find(key);
        return v && v->is_number() ? v->AsDouble(fallback) : fallback;
    };
    o.echo_delay_s = num("echo_delay_s", o.echo_delay_s);
    o.echo_offset_m = num("echo_offset_m", o.echo_offset_m);
    // A hand-edited or damaged file may hold any number: out-of-range (or NaN) values fall
    // back before the cast, which is undefined for doubles an int cannot hold.
    auto whole = [&](const char* key, int fallback, int lo, int hi) {
        const double v = num(key, fallback);
        return v >= lo && v <= hi ? static_cast<int>(v) : fallback;
    };
    o.send_hz = whole("send_hz", o.send_hz, 1, 120);
    o.interp_delay_ms = whole("interp_delay_ms", o.interp_delay_ms, 0, 1000);
    o.partner_activate = d->Bool("partner_activate", o.partner_activate);
    o.verbose = d->Bool("verbose", o.verbose);
    o.debug_menu = d->Bool("debug_menu", o.debug_menu);
    o.dev_tools = d->Bool("dev_tools", o.dev_tools);
    o.mouse_lock = d->Bool("mouse_lock", o.mouse_lock);
    const int key = ParseFunctionKey(d->Str("debug_menu_key"));
    if (key) o.debug_menu_key = key;
    // Only an explicit "holo" picks the hologram: missing or unknown keeps the default (real).
    o.real_partner_body = d->Str("partner_body") != "holo";
    o.self_character = d->Bool("self_character", o.self_character);
    // Only an explicit "sam" keeps Sam's voice: missing or unknown keeps the default (auto).
    o.mute_sam_voice = d->Str("self_voice") != "sam";
    o.partner_voice_sync = d->Bool("voice_sync", o.partner_voice_sync);
    o.partner_sound_sync = d->Bool("sound_sync", o.partner_sound_sync);
    o.partner_volume = whole("partner_volume", o.partner_volume, 0, 100);
    o.name_tags = d->Bool("name_tags", o.name_tags);
    o.partner_collision = d->Bool("partner_collision", o.partner_collision);
    o.partner_vehicle_damage = d->Bool("partner_vehicle_damage", o.partner_vehicle_damage);
    o.allow_other_characters = d->Bool("allow_other_characters", o.allow_other_characters);
    o.host_save = d->Bool("host_save", o.host_save);
    o.backup_host_saves = d->Bool("backup_host_saves", o.backup_host_saves);
    o.share_world = d->Bool("share_world", o.share_world);
    return ClampDebugOptions(o);
}

void DebugOptionsToSettings(const DebugOptions& o, json::Value& settings) {
    json::Value d = json::Value::Object();
    d["echo_delay_s"] = o.echo_delay_s;
    d["echo_offset_m"] = o.echo_offset_m;
    d["send_hz"] = o.send_hz;
    d["interp_delay_ms"] = o.interp_delay_ms;
    d["partner_activate"] = o.partner_activate;
    d["verbose"] = o.verbose;
    d["debug_menu"] = o.debug_menu;
    d["dev_tools"] = o.dev_tools;
    d["mouse_lock"] = o.mouse_lock;
    d["debug_menu_key"] = FunctionKeyName(o.debug_menu_key);
    d["partner_body"] = o.real_partner_body ? "real" : "holo";
    d["self_character"] = o.self_character;
    d["self_voice"] = o.mute_sam_voice ? "auto" : "sam";
    d["voice_sync"] = o.partner_voice_sync;
    d["sound_sync"] = o.partner_sound_sync;
    d["partner_volume"] = o.partner_volume;
    d["name_tags"] = o.name_tags;
    d["partner_collision"] = o.partner_collision;
    d["partner_vehicle_damage"] = o.partner_vehicle_damage;
    d["allow_other_characters"] = o.allow_other_characters;
    d["host_save"] = o.host_save;
    d["backup_host_saves"] = o.backup_host_saves;
    d["share_world"] = o.share_world;
    settings["debug"] = d;
}

std::string GuestPickFromSettings(const json::Value& settings, const std::string& stored) {
    // (The keys compare as the mod finds them, without case: a hand-edited "Sam" is the old default too.)
    const std::string pick = GuestPickOrDefault(stored);
    if (settings.Int("character_defaults", 0) < kCharacterDefaults && (stored.empty() || pick == kHostCharacter)) return kGuestCharacter;
    return pick;
}

static_assert(kLocalTestFpsDefault == static_cast<int>(st::sandbox::kLocalTestFps), "the launcher's and the testenv's Local test frame rate");
int LocalTestFps(int stored) { return stored == 30 || stored == 60 || stored == 120 || stored == 0 ? stored : kLocalTestFpsDefault; }
unsigned LocalTestGameFps(int choice) {
    const int c = LocalTestFps(choice);
    return c == 0 ? st::sandbox::kGameMaxFps : static_cast<unsigned>(c);
}
int LocalTestFrameCap(int choice) { return LocalTestFps(choice); }

void MakeLocalTestInis(const DebugOptions& options, const LocalTestChoices& choices, SessionIni& host, SessionIni& guest) {
    host = SessionIni{};
    host.role = "host";
    host.name = choices.name;
    host.my_character = kHostCharacter;   // the host game stays Sam
    ApplyDebugOptions(options, host);
    host.secret = choices.secret.empty() ? NewSecret() : choices.secret;
    host.transport = "udp";
    host.udp_peer = "127.0.0.1";
    host.udp_port_local = kDefaultUdpPort;
    host.udp_port_peer = kDefaultUdpPort + 1;
    host.debug_menu = true;   // the input split lives in the overlay
    host.sandbox_saves = kLocalHostSandbox;
    host.sandbox_foreign_saves = true;
    host.appdir_settings = true;
    host.local_input = choices.host_pad ? "host_pad" : "host";
    host.frame_cap = LocalTestFrameCap(LocalTestFps(choices.fps));   // v0.23: 60 by default (was 30)
    host.local_window = "left";   // the two windows side by side
    // The host-held save (the Debug option, default on): the session the online Host / Join play, so the local test
    // shows the waiting screen, the save transfer, the shared vehicles, riding together and vehicle hits. The host
    // plays its local-host copy (never the real saves); the guest's local-guest folder is wiped like coop-guest.
    host.host_save = options.host_save;
    // Two windows side by side: the cursor stays in the one in front (the Debug option, default on; Local test only).
    host.mouse_lock = options.mouse_lock;
    // Shared structures, ladders and ropes (docs/world-sync.md): the Debug option (default on), as for the online sessions.
    host.world_sync = options.share_world;
    guest = host;
    guest.role = "guest";
    guest.udp_port_local = kDefaultUdpPort + 1;
    guest.udp_port_peer = kDefaultUdpPort;
    guest.sandbox_saves = kLocalGuestSandbox;
    guest.second_instance = true;
    guest.local_input = "guest";
    guest.local_window = "right";
    guest.my_character = GuestCharacter(choices.guest_character, options.allow_other_characters);
}

void ApplyDebugOptions(const DebugOptions& o, SessionIni& ini) {
    ini.echo_delay_s = o.echo_delay_s;
    ini.echo_offset_m = o.echo_offset_m;
    ini.send_hz = o.send_hz;
    ini.interp_delay_ms = o.interp_delay_ms;
    ini.partner_activate = o.partner_activate;
    ini.verbose = o.verbose;
    ini.debug_menu = o.debug_menu;
    ini.debug_menu_key = o.debug_menu_key;
    ini.dev_tools = o.dev_tools;
    ini.partner_body = o.real_partner_body ? "real" : "holo";
    // The self view needs the pose hook, which only partner_body=real installs; the writer adds
    // the key only for a non-host whose character is not Sam.
    ini.self_character = o.self_character && o.real_partner_body;
    // auto: the mod mutes Sam's voice only when this player's character is not Sam (never for a host).
    ini.self_voice = o.mute_sam_voice ? "auto" : "sam";
    ini.voice_sync = o.partner_voice_sync;
    ini.sound_sync = o.partner_sound_sync;
    ini.partner_volume = o.partner_volume;

    ini.name_tags = o.name_tags;
    ini.partner_collision = o.partner_collision;
    ini.partner_vehicle_damage = o.partner_vehicle_damage;
    ini.one_shot = true;
}

namespace {

const char* UpdateStatusName(UpdateStatus s) {
    switch (s) {
    case UpdateStatus::idle: return "idle";
    case UpdateStatus::checking: return "checking";
    case UpdateStatus::up_to_date: return "up_to_date";
    case UpdateStatus::available: return "available";
    case UpdateStatus::downloading: return "downloading";
    case UpdateStatus::applying: return "applying";
    case UpdateStatus::ready_to_restart: return "ready_to_restart";
    case UpdateStatus::error: return "error";
    }
    return "?";
}

bool UpdateBusy(UpdateStatus s) {
    return s == UpdateStatus::checking || s == UpdateStatus::downloading || s == UpdateStatus::applying || s == UpdateStatus::ready_to_restart;
}

class RealModel final : public LauncherModel {
public:
    RealModel(const std::wstring& exe_dir, std::shared_ptr<Env> env, const ModelStart& start, UpdateSeams seams)
        : env_(std::move(env)), paths_(GetInstancePaths(exe_dir)), start_(start), seams_(std::move(seams)) {
        ReadSettings(paths_, settings_);
        LoadChoices();
        crash_present_ = IsFile(paths_.crash_report);
        Recheck();
        LauncherLog(*env_, paths_, std::string("window=open phase=") + PhaseName(phase_));
        InitUpdate();
    }

    ~RealModel() override {
        update_cancel_ = true;   // a download stops at its next read
        if (update_thread_.joinable()) update_thread_.join();
    }

    Phase phase() const override { return phase_; }
    std::string version() const override { return ST_LAUNCHER_VERSION; }
    std::string game_dir() const override { return Utf8(paths_.game); }
    std::string discovered_game_dir() const override { return discovered_; }
    InstallChecks checks() const override { return checks_; }
    std::string last_error() const override { return last_error_; }
    void recheck() override { Recheck(); }

    Transport transport() const override { return transport_; }
    void set_transport(Transport t) override {
        transport_ = t;
        settings_["transport"] = TransportName(t);
        SaveChoices();
    }
    LanSettings lan() const override { return lan_; }
    void set_lan(const LanSettings& lan) override {
        lan_ = lan;
        settings_["udp_peer"] = lan.peer_ip;
        settings_["udp_port_local"] = lan.port_local;
        settings_["udp_port_peer"] = lan.port_peer;
        SaveChoices();
    }
    std::string player_name() const override { return name_; }
    void set_player_name(const std::string& name) override {
        name_ = name;
        settings_["name"] = name;
        SaveChoices();
    }
    // The stored pick is kept as it is; the guest plays GuestCharacter(pick, option). With other
    // characters off the choice is fixed: nothing to set.
    std::string my_character() const override { return GuestCharacter(my_character_, debug_.allow_other_characters); }
    void set_my_character(const std::string& key) override {
        if (!debug_.allow_other_characters) return;
        my_character_ = GuestPickOrDefault(key);
        settings_["my_character"] = my_character_;
        SaveChoices();
    }

    bool host(std::string& code_out, std::string& error_out) override {
        code_out.clear();
        if (!ReadyFor(error_out)) return false;
        if (!SteamClientRunning(*env_)) {
            error_out = "Start Steam and log in first. Your join code carries your Steam account.";
            return false;
        }
        const std::uint64_t self = OwnSteamId(*env_);
        const std::string secret = NewSecret();
        const std::string code = FormatJoinCode(secret, self);
        if (code.empty()) {
            error_out = "Could not make a join code from your Steam account. Restart Steam, then try again.";
            return false;
        }
        SessionIni ini = BaseIni("host");
        ini.secret = secret;
        ini.host_save = debug_.host_save;   // the host plays on its own saves either way; the mod then offers them to the guest
        ini.world_sync = debug_.share_world;   // shared structures, ladders and ropes (written with host_save)
        if (!ApplyTransport(ini, error_out)) return false;
        if (!StartSession(ini, Role::host, error_out)) return false;
        code_ = code;
        code_out = code;
        return true;
    }

    CodeCheck check_code(const std::string& code) const override { return CheckJoinCode(code, OwnSteamId(*env_)); }

    bool join(const std::string& code, std::string& error_out) override {
        if (!ReadyFor(error_out)) return false;
        const CodeCheck check = check_code(code);
        if (!check.ok) {
            error_out = check.message.empty() ? "Paste the code your partner sent you." : check.message;
            return false;
        }
        std::string secret;
        std::uint64_t host = 0;
        if (!ParseJoinCode(code, secret, host)) {
            error_out = "That code doesn't work. Ask your partner to send it again.";
            return false;
        }
        SessionIni ini = BaseIni("guest");
        ini.secret = secret;
        ini.peer_steamid = Decimal(host);
        if (!ApplyTransport(ini, error_out)) return false;
        // The host-held save: this game runs only on the private coop-guest folder (never on your own saves).
        if (debug_.host_save && !PrepareCoopGuest(ini, error_out)) return false;
        ini.world_sync = debug_.share_world;   // shared structures, ladders and ropes: both games must have it (the mod's HELLO)
        return StartSession(ini, Role::join, error_out);
    }

    bool solo_test(std::string& error_out) override {
        if (!ReadyFor(error_out)) return false;
        // The echo needs no partner: loopback UDP, so nothing reaches the network or Steam.
        SessionIni ini = BaseIni("echo");
        Loopback(ini);
        return StartSession(ini, Role::echo, error_out);
    }

    bool debug_solo(std::string& error_out) override {
        if (!ReadyFor(error_out)) return false;
        // No partner and no network; loopback in case the mod opens its transport anyway.
        SessionIni ini = BaseIni("solo");
        ini.debug_menu = true;   // the menu is the point of "debug tools only", whatever the checkbox says
        Loopback(ini);
        return StartSession(ini, Role::solo, error_out);
    }

    bool cancel_launch(std::string& error_out) override {
        if (phase_ != Phase::launching) {
            error_out = "The game is already running. Quit it from the game's menu.";
            return false;
        }
        if (AnyGameRunningIn(*env_, paths_.game)) {
            error_out = "The game has already started. Quit it from the game's menu; co-op tidies up after it.";
            return false;
        }
        const CleanupResult r = CleanupSession(*env_, paths_);
        if (!r.ran) {
            error_out = r.message;
            return false;
        }
        LauncherLog(*env_, paths_, "launch=cancelled");
        EndSessionState();
        phase_ = Phase::ready;
        return true;
    }

    bool end_session(std::string& error_out) override {
        if (AnyGameRunningIn(*env_, paths_.game)) {
            error_out = "DEATH STRANDING is running. Quit it from the game's menu first; co-op tidies up after it.";
            return false;
        }
        const CleanupResult r = CleanupSession(*env_, paths_);
        if (!r.ran) {
            error_out = r.message;
            return false;
        }
        if (r.ini_kept) {
            error_out = r.message;
            return false;
        }
        EndSessionState();
        if (phase_ == Phase::launching || phase_ == Phase::in_game || phase_ == Phase::after_game) phase_ = Phase::ready;
        pending_ = false;
        return true;
    }

    Role session_role() const override { return role_; }
    Transport session_transport() const override { return session_transport_; }
    std::string join_code() const override { return role_ == Role::host && SessionPhase() ? code_ : std::string(); }
    LinkView link() const override { return link_; }
    bool session_pending() const override { return pending_; }

    std::string mod_dir() const override { return Utf8(paths_.mod); }
    std::string mod_log_path() const override { return Utf8(paths_.mod_log); }

    DebugOptions debug_options() const override { return debug_; }
    void set_debug_options(const DebugOptions& options) override {
        debug_ = ClampDebugOptions(options);
        DebugOptionsToSettings(debug_, settings_);
        SaveChoices();
    }
    std::string mod_log_tail(std::size_t max_bytes) const override {
        if (!IsFile(paths_.mod_log)) return {};
        return ReadTail(paths_.mod_log, max_bytes);
    }
    bool has_crash_report() const override { return crash_present_; }
    bool crash_report(std::string& text_out) const override {
        text_out.clear();
        return IsFile(paths_.crash_report) && ReadTextShared(paths_.crash_report, text_out, 4u << 20);
    }

    bool set_mod_enabled(bool enabled, std::string& error_out) override {
        if (SessionPhase()) {
            error_out = "A game session is running. Switch the mod on or off after the game has closed.";
            return false;
        }
        if (UpdateRunning()) {
            error_out = kUpdateRunningText;
            return false;
        }
        if (!SetModEnabled(*env_, paths_, package_, enabled, error_out)) return false;
        LauncherLog(*env_, paths_, std::string("mod ") + (enabled ? "enabled" : "disabled"));
        Recheck();
        return true;
    }

    bool clear_session(std::string& error_out) override {
        if (AnyGameRunningIn(*env_, paths_.game)) {
            error_out = "DEATH STRANDING is running. Quit it from the game's menu first.";
            return false;
        }
        const CleanupResult r = CleanupSession(*env_, paths_);
        if (!r.ran) {
            error_out = r.message;
            return false;
        }
        // Also a session.ini written by hand or by tools\deploy.ps1: the player asked for it.
        if (IsFile(paths_.session_ini) && !DeleteFileChecked(paths_.session_ini)) {
            error_out = "session.ini could not be deleted (is it open in another program?).";
            return false;
        }
        LauncherLog(*env_, paths_, "session=cleared");
        EndSessionState();
        if (SessionPhase()) phase_ = Phase::ready;
        pending_ = false;
        return true;
    }

    bool collect_logs(std::string& path_out, std::string& error_out) override {
        std::string summary;
        summary += "Stranded Together launcher " + version() + "\n";
        summary += "Windows " + env_->OsVersion() + "\n";
        summary += std::string("phase=") + PhaseName(phase_) + "\n";
        summary += std::string("game_found=") + (checks_.game_found ? "1" : "0") + " version_tested=" + (checks_.version_tested ? "1" : "0") +
                   " mod_present=" + (checks_.mod_present ? "1" : "0") + " steam_running=" + (checks_.steam_running ? "1" : "0") + "\n";
        summary += std::string("mod_state=") + ModStateName(checks_.mod_state) + " package=" +
                   (package_.present ? (package_.ok ? package_.build_id : std::string("damaged")) : std::string("none")) + "\n";
        summary += "debug: echo_delay_s=" + std::to_string(debug_.echo_delay_s) + " echo_offset_m=" + std::to_string(debug_.echo_offset_m) +
                   " send_hz=" + std::to_string(debug_.send_hz) + " interp_delay_ms=" + std::to_string(debug_.interp_delay_ms) +
                   " partner_activate=" + (debug_.partner_activate ? "1" : "0") + " verbose=" + (debug_.verbose ? "1" : "0") +
                   " debug_menu=" + (debug_.debug_menu ? "1" : "0") + " key=" + FunctionKeyName(debug_.debug_menu_key) +
                   " host_save=" + (debug_.host_save ? "1" : "0") + " backup_host_saves=" + (debug_.backup_host_saves ? "1" : "0") + "\n";
        summary += std::string("transport=") + TransportName(transport_) + "\n";
        {
            const UpdateInfo u = update_info();
            summary += std::string("update=") + UpdateStatusName(u.status) + " auto_check=" + (u.auto_check ? "1" : "0") +
                       (u.latest_version.empty() ? std::string() : " latest=" + u.latest_version) + "\n";
        }
        if (SessionPhase()) summary += "session_role=" + RoleIniName(role_) + " link_state=" + link_.state + "\n";
        if (!last_error_.empty()) summary += "last_error=" + last_error_ + "\n";
        const bool ok = CollectLogs(*env_, paths_, summary, path_out, error_out);
        LauncherLog(*env_, paths_, std::string("collect_logs ok=") + (ok ? "1" : "0"));
        return ok;
    }

    // ---- Updates ------------------------------------------------------------------------
    UpdateInfo update_info() const override {
        std::lock_guard<std::mutex> lock(update_mutex_);
        UpdateInfo info = update_;
        info.updated_to = updated_to_;
        info.auto_check = auto_check_;
        return info;
    }

    // ---- Debug > Test saves -----------------------------------------------------------------
    TestSaveView test_save() const override {
        TestSaveView v;
        v.folder = test_folder_;
        v.folder_ok = test_check_.ok;
        v.folder_note = test_folder_.empty() ? std::string("No test save picked yet.") : test_check_.note;
        v.use_for_all = test_use_;
        v.seeded = test_seeded_;
        v.sandbox_dir = Utf8(JoinPath2(paths_.mod, L"sandbox"));
        v.result = test_result_;
        v.tone = test_tone_;
        return v;
    }

    bool set_test_save_folder(const std::string& folder, std::string& error_out) override {
        if (folder.empty()) {
            test_folder_.clear();
            test_check_ = SeedCheck{};
            SaveTestChoices();
            return true;
        }
        // The launcher's own copies are deleted below when the folder changes (and by Reset test saves):
        // picking one would delete the save the user just picked.
        if (PathInside(FullPath(Wide(folder)), FullPath(JoinPath2(paths_.mod, L"sandbox")))) {
            error_out = "That folder is one of the launcher's own test copies, which it deletes when you reset or change the test save. "
                        "Pick the original save folder instead (for example the one you downloaded).";
            return false;
        }
        const SeedCheck check = CheckSeedFolder(Wide(folder));
        if (!check.ok) {
            error_out = check.note;
            return false;
        }
        if (local_.running || SessionPhase() || AnyGameRunningIn(*env_, paths_.game)) {
            error_out = "Quit DEATH STRANDING first: the test save copy is in use.";
            return false;
        }
        const bool changed = !IEquals(Wide(folder), Wide(test_folder_));
        test_folder_ = folder;
        test_check_ = check;
        SaveTestChoices();
        if (changed) {
            // A new test save: the old copies were seeded from the previous folder.
            std::string ignored;
            for (const char* name : {kTestSaveSandbox, kLocalHostSandbox, kLocalGuestSandbox})
                ResetSandbox(paths_, GetSandboxPaths(paths_, name), ignored);
        }
        RefreshTestSave();
        LauncherLog(*env_, paths_, "test_save=folder_set slots=" + std::to_string(check.slots));
        return true;
    }

    void set_use_test_save(bool on) override {
        test_use_ = on;
        SaveTestChoices();
    }

    bool play_test_save(std::string& error_out) override {
        if (!ReadyFor(error_out)) return false;
        SessionIni ini = BaseIni("solo");
        ini.debug_menu = true;
        Loopback(ini);
        if (!PrepareSandbox(ini, kTestSaveSandbox, error_out)) return false;
        return StartSession(ini, Role::solo, error_out);
    }

    bool reset_test_saves(std::string& error_out) override {
        if (local_.running || SessionPhase() || AnyGameRunningIn(*env_, paths_.game)) {
            error_out = "Quit DEATH STRANDING first: the test save copies are in use.";
            return false;
        }
        for (const char* name : {kTestSaveSandbox, kLocalHostSandbox, kLocalGuestSandbox})
            if (!ResetSandbox(paths_, GetSandboxPaths(paths_, name), error_out)) return false;
        RefreshTestSave();
        test_result_.clear();
        LauncherLog(*env_, paths_, "test_save=reset");
        return true;
    }

    LocalTestView local_test() const override {
        LocalTestView v;
        v.running = local_.running;
        v.host_pid = local_.host_pid;
        v.guest_pid = local_.guest_pid;
        v.host_state = local_.host_state;
        v.guest_state = local_.guest_state;
        v.headline = local_.headline;
        v.detail = local_.detail;
        v.features_off = local_.running ? local_.features_off : std::string();
        v.tone = local_.tone;
        v.host_pad = local_host_pad_;
        v.fps = local_fps_;
        v.guest_character = GuestCharacter(local_guest_character_, debug_.allow_other_characters);
        return v;
    }

    void set_local_test_host_pad(bool on) override {
        local_host_pad_ = on;
        SaveTestChoices();
    }
    void set_local_test_fps(int fps) override {
        local_fps_ = LocalTestFps(fps);
        SaveTestChoices();
    }
    void set_local_test_guest_character(const std::string& key) override {
        if (!debug_.allow_other_characters) return;   // fixed: Fragile (porter suit)
        local_guest_character_ = GuestPickOrDefault(key);
        SaveTestChoices();
    }

    bool start_local_test(std::string& error_out) override {
        if (!ReadyFor(error_out)) return false;
        if (local_.running) {
            error_out = "The local two-player test is already running.";
            return false;
        }
        if (!SteamClientRunning(*env_)) {
            error_out = "Start Steam and log in first: both games sign in with your Steam account.";
            return false;
        }
        // Both games: their own sandbox copy (the test save, or a copy of your own saves), low-quality
        // windowed graphics, a frame cap, loopback UDP between them, the input split.
        const SeedCheck* seed = test_check_.ok ? &test_check_ : nullptr;
        const std::wstring real = RealSaveRoot();
        const bool host_save = debug_.host_save;
        SessionGuestReport guest_report;
        for (const char* name : {kLocalHostSandbox, kLocalGuestSandbox}) {
            const SandboxPaths sp = GetSandboxPaths(paths_, name);
            const bool guest_folder = std::string(name) == kLocalGuestSandbox;
            if (guest_folder && host_save) {
                // The host-held save: the guest game gets the host's save over the link, as an online guest does. Its
                // folder is prepared like coop-guest every time (slots and incoming\ wiped, your profile kept).
                if (!SeedSessionGuest(paths_, real, Account(), guest_report, error_out, kLocalGuestSandbox)) return false;
            } else if (!SandboxSeeded(sp) || (guest_folder && SandboxIsSessionGuest(sp))) {
                // (a guest folder left without slots by a host-save local test gets its test-save copy back)
                if (!SeedSandbox(paths_, sp, seed, real, Account(), seed ? test_folder_ : std::string("your own saves"), error_out))
                    return false;
            }
            std::string werr;
            // Every time: the game rewrites this file on exit (fullscreen again, its own size).
            std::string current;
            if (IsFile(sp.settings)) ReadTextShared(sp.settings, current);
            if (!WriteAtomic(sp.settings, LocalTestSettingsCfg(current, LocalTestGameFps(local_fps_)), true, werr)) {
                error_out = "Could not write the test games' graphics settings: " + werr;
                return false;
            }
        }
        LocalTestChoices choices;
        choices.name = name_;
        choices.guest_character = local_guest_character_;
        choices.host_pad = local_host_pad_;
        choices.fps = local_fps_;
        SessionIni host, guest;
        MakeLocalTestInis(debug_, choices, host, guest);
        for (const auto& [ini, dir] : {std::pair<const SessionIni*, std::wstring>{&host, LocalDir(L"host")}, {&guest, LocalDir(L"guest")}}) {
            std::string text, werr;
            if (!MakeSessionIniText(*ini, text, error_out)) return false;
            CreateDirectories(dir);
            for (const wchar_t* stale : {L"link-status.json", L"sandbox-refused.txt", L"sandbox-active.txt"}) DeleteFileChecked(JoinPath2(dir, stale));
            if (!WriteAtomic(JoinPath2(dir, L"session.ini"), text, true, werr)) {
                error_out = "Could not write " + Utf8(JoinPath2(dir, L"session.ini")) + ": " + werr;
                return false;
            }
        }
        real_before_ = SaveTreeManifest(real);
        std::uint32_t pid = 0;
        if (!env_->StartProcess(paths_.exe, paths_.game, LocalEnv(L"host"), pid) || !pid) {
            error_out = "DEATH STRANDING could not be started for the local test.";
            CleanupLocalIni();
            return false;
        }
        local_ = LocalTest{};
        local_.running = true;
        local_.host_save = host.host_save;
        local_.host_pid = pid;
        local_.started_ms = env_->NowMs();
        local_.guest_due_ms = local_.started_ms + kLocalGuestDelayMs;
        local_.headline = "Starting the first game (host)...";
        local_.detail = "The second game (guest) starts a few seconds later. Each window shows the other player.";
        local_.tone = LinkView::Tone::neutral;
        test_result_.clear();
        LauncherLog(*env_, paths_, "local_test=start host_pid=" + std::to_string(pid) + (local_host_pad_ ? " host_pad=1" : "") +
                                       " fps=" + std::to_string(LocalTestGameFps(local_fps_)) + " guest_character=" + guest.my_character + " partner_body=" + host.partner_body +
                                       " host_save=" + (host.host_save ? "1" : "0") +
                                       (host.host_save ? " guest_wiped_slots=" + std::to_string(guest_report.wiped_slots) : std::string()));
        return true;
    }

    void set_auto_update_check(bool on) override {
        {
            std::lock_guard<std::mutex> lock(update_mutex_);
            auto_check_ = on;
        }
        settings_["auto_update_check"] = on;
        SaveChoices();
        LauncherLog(*env_, paths_, std::string("update auto_check=") + (on ? "1" : "0"));
    }

    void check_for_updates() override {
        {
            std::lock_guard<std::mutex> lock(update_mutex_);
            if (UpdateBusy(update_.status)) return;
            update_.status = UpdateStatus::checking;
            update_.error.clear();
            update_.error_after_check = false;
        }
        JoinUpdateThread();
        const std::string feed = FeedUrl();
        FeedOptions options;
        options.local_feed = feed != kReleasesUrl;
        const std::string current = version();
        RunUpdateJob([this, feed, options, current]() {
            std::string body, error;
            UpdateInfo found;
            const bool ok = Http().Get(feed, kMaxFeedBytes, body, error) && ParseReleasesFeed(body, current, found, error, options);
            std::lock_guard<std::mutex> lock(update_mutex_);
            if (!ok) {
                LauncherLog(*env_, paths_, "update=check_failed; error=" + error);
                update_.status = UpdateStatus::error;
                update_.error = "Could not check for updates. Check your internet connection, then try again.";
                update_.error_after_check = true;
                update_.download_url.clear();
                return;
            }
            update_ = found;
            LauncherLog(*env_, paths_, std::string("update=checked; status=") + UpdateStatusName(found.status) +
                                           (found.latest_version.empty() ? std::string() : "; latest=" + found.latest_version));
        });
    }

    void start_update() override {
        UpdateInfo target;
        {
            std::lock_guard<std::mutex> lock(update_mutex_);
            const bool retry = update_.status == UpdateStatus::error && !update_.error_after_check;
            if ((update_.status != UpdateStatus::available && !retry) || update_.download_url.empty() || update_.latest_version.empty()) return;
            const std::string blocked = UpdateBlocker();
            if (!blocked.empty()) {
                update_.status = UpdateStatus::error;
                update_.error = blocked;
                update_.error_after_check = false;
                LauncherLog(*env_, paths_, "update=refused; reason=game_or_session");
                return;
            }
            update_.status = UpdateStatus::downloading;
            update_.bytes_downloaded = 0;
            update_.progress = 0.0f;
            update_.error.clear();
            target = update_;
        }
        JoinUpdateThread();
        update_cancel_ = false;
        std::vector<std::wstring> args = {L"--after-update", std::to_wstring(env_->CurrentPid())};
        if (FeedUrl() != kReleasesUrl) {
            args.push_back(L"--update-feed");
            args.push_back(Wide(FeedUrl()));
        }
        LauncherLog(*env_, paths_, "update=start; version=" + target.latest_version + "; asset=" + target.asset_name);
        RunUpdateJob([this, target, args]() { UpdateJob(target, args); });
    }

    void tick() override {
        if (start_.check_on_start && !auto_checked_ && phase_ != Phase::not_in_game_folder) {
            auto_checked_ = true;   // at most once per launch; Check for updates is always there
            bool check = !start_.update_feed.empty();
            {
                std::lock_guard<std::mutex> lock(update_mutex_);
                check = check || auto_check_;
            }
            if (check) check_for_updates();
        }
        // Developer --update-now (with a loopback --update-feed): install what the check found, once.
        if (start_.update_now && !update_now_started_ && !start_.update_feed.empty() && update_info().status == UpdateStatus::available) {
            update_now_started_ = true;
            LauncherLog(*env_, paths_, "update=auto; reason=update_now");
            start_update();
        }
        const std::int64_t now = env_->NowMs();
        if (now >= next_check_ms_) {
            next_check_ms_ = now + kCheckEveryMs;
            checks_.steam_running = SteamClientRunning(*env_);
            crash_present_ = IsFile(paths_.crash_report);
            if (phase_ == Phase::error || phase_ == Phase::ready) {
                // The DLL, its .disabled twin, package.json or the exe changed: check them again.
                if (ModSignature() != mod_sig_) Recheck();
                else if (phase_ == Phase::ready) AdoptSession();   // another window may have started a game
            }
            json::Value r;
            const bool active = ReadSessionRecord(paths_, r) && RecordIsActive(r);
            pending_ = active || IsFile(paths_.session_ini);
            record_lost_ = SessionPhase() && !(active && r.Str("session_id") == session_id_);
        }
        TickLocalTest(now);
        if (session_sandbox_ && (phase_ == Phase::launching || phase_ == Phase::in_game) && IsFile(SandboxRefusedPath())) {
            // The mod ended the game in DllMain: the test-save redirect could not be installed.
            FinishSandboxCheck();
            CleanupSession(*env_, paths_);
            EndSessionState();
            phase_ = Phase::ready;
            pending_ = false;
        }
        if (session_sandbox_ && phase_ == Phase::in_game && sandbox_check_at_ && now >= sandbox_check_at_) {
            sandbox_check_at_ = 0;
            if (!IsFile(SandboxActivePath()) && !IsFile(SandboxRefusedPath())) {
                sandbox_unconfirmed_ = true;
                test_tone_ = TestSaveView::Tone::bad;
                test_result_ = "WARNING: the running game has not confirmed the test save (no sandbox-active.txt from the mod). It may be "
                               "running on your own saves: quit it without saving.";
                LauncherLog(*env_, paths_, "test_save=unconfirmed (no sandbox-active.txt " + std::to_string(kSandboxConfirmMs / 1000) +
                                               " s after the game started)");
            }
        }
        switch (phase_) {
        case Phase::launching: {
            if (AnyGameRunningIn(*env_, paths_.game)) {
                LauncherLog(*env_, paths_, "game=started");
                phase_ = Phase::in_game;
                gone_since_ = 0;
                if (session_sandbox_) sandbox_check_at_ = now + kSandboxConfirmMs;
            } else if (record_lost_) {
                // The helper gave up (the game never started) or another window cancelled.
                EndSessionState();
                phase_ = Phase::ready;
                last_error_.clear();
            }
            PollStatus(now);
            break;
        }
        case Phase::in_game: {
            if (AnyGameRunningIn(*env_, paths_.game)) {
                gone_since_ = 0;
            } else {
                if (!gone_since_) gone_since_ = now;
                if (now - gone_since_ >= kExitConfirmMs) {
                    LauncherLog(*env_, paths_, "game=exited");
                    CleanupSession(*env_, paths_);   // the helper may have done it already
                    FinishSandboxCheck();
                    FinishHostCheck();
                    phase_ = Phase::after_game;
                    after_until_ = now + kAfterGameMs;
                    link_ = LinkView{};
                    link_.headline = "The game has closed";
                    link_.detail = "Co-op is switched off again. Starting DEATH STRANDING from Steam now runs it unmodded.";
                    pending_ = false;
                    break;
                }
            }
            PollStatus(now);
            break;
        }
        case Phase::after_game:
            if (now >= after_until_) {
                EndSessionState();
                phase_ = Phase::ready;
            }
            break;
        default: break;
        }
    }

private:
    // ---- Updates ------------------------------------------------------------------------
    void InitUpdate() {
        auto_check_ = settings_.Bool("auto_update_check", true);
        update_.current_version = version();
        // What an update left behind: StrandedTogether.exe.old (the old launcher may still be exiting
        // right after a handoff), and *.st-new / *.st-old of an update that was ended half way.
        const bool old_gone = CleanOldExecutable(paths_, start_.after_update ? 5000 : 0);
        const std::size_t leftovers = IsFile(paths_.exe) ? CleanUpdateLeftovers(paths_) : 0;
        if (start_.after_update) {
            SemVer v;
            updated_to_ = ParseSemVer(version(), v) ? FormatSemVer(v) : version();
            LauncherLog(*env_, paths_, "update=finished; version=" + version() + (old_gone ? "" : "; old_exe=kept"));
        } else if (!old_gone) {
            LauncherLog(*env_, paths_, "update=old_exe_kept");
        }
        if (leftovers) LauncherLog(*env_, paths_, "update=leftovers_cleaned; count=" + std::to_string(leftovers));
        if (!start_.update_feed.empty() && !IsLoopbackUrl(start_.update_feed)) {
            LauncherLog(*env_, paths_, "update=feed_refused (not a loopback url)");
            start_.update_feed.clear();
        }
    }

    std::string FeedUrl() const { return start_.update_feed.empty() ? std::string(kReleasesUrl) : start_.update_feed; }

    HttpClient& Http() {
        std::call_once(http_once_, [this] {
            if (!seams_.http) seams_.http = MakeWinHttpClient("StrandedTogether-Launcher/" + version());
        });
        return *seams_.http;
    }

    void JoinUpdateThread() {
        if (update_thread_.joinable()) update_thread_.join();
    }

    void RunUpdateJob(std::function<void()> job) {
        if (seams_.synchronous) {
            job();
            return;
        }
        update_thread_ = std::thread(std::move(job));
    }

    // "" when Update now may install here, otherwise the player's sentence.
    std::string UpdateBlocker() const {
        if (phase_ == Phase::not_in_game_folder) return "Updates install into the game folder. Move the launcher there first.";
        if (SessionPhase() || AnyGameRunningIn(*env_, paths_.game))
            return "Quit DEATH STRANDING first: the update replaces files the game uses. Then press Update now again.";
        return {};
    }

    // On the update thread (or inline): download, check, install, restart.
    void UpdateJob(const UpdateInfo& target, const std::vector<std::wstring>& args) {
        auto fail = [this](const std::string& sentence) {
            std::lock_guard<std::mutex> lock(update_mutex_);
            update_.status = UpdateStatus::error;
            update_.error = sentence;
            update_.error_after_check = false;
        };
        const std::wstring zip = JoinPath2(paths_.mod, L"update.zip");
        CreateDirectories(paths_.mod);
        std::string error;
        const std::uint64_t limit = target.download_size ? std::min<std::uint64_t>(target.download_size, kMaxUpdateZipBytes) : kMaxUpdateZipBytes;
        const bool downloaded = Http().Download(target.download_url, zip, limit, [this](const HttpProgress& p) {
            std::lock_guard<std::mutex> lock(update_mutex_);
            update_.bytes_downloaded = p.bytes;
            if (p.total) update_.download_size = p.total;
            update_.progress = update_.download_size ? static_cast<float>(static_cast<double>(p.bytes) / static_cast<double>(update_.download_size)) : 0.0f;
        }, &update_cancel_, error);
        if (update_cancel_) {
            DeleteFileChecked(zip);
            return;   // the window is closing
        }
        if (!downloaded) {
            LauncherLog(*env_, paths_, "update=download_failed; error=" + error);
            DeleteFileChecked(zip);
            fail("The update could not be downloaded. Check your internet connection, then try again.");
            return;
        }
        // The feed's own size and SHA-256 (GitHub's asset digest), when it gave them.
        std::string hex;
        std::uint64_t size = 0;
        const bool hashed = Sha256FileHex(zip, hex, &size);
        if (!hashed || (target.download_size && size != target.download_size) || (!target.asset_sha256.empty() && hex != target.asset_sha256)) {
            LauncherLog(*env_, paths_, std::string("update=download_mismatch; size=") + std::to_string(size) + (hashed ? "" : "; unreadable"));
            DeleteFileChecked(zip);
            fail("The downloaded update is damaged or incomplete. Nothing was changed. Try again later.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(update_mutex_);
            update_.status = UpdateStatus::applying;
            update_.progress = 1.0f;
        }
        ApplyOptions options;
        const std::shared_ptr<Env> env = env_;
        const std::wstring game = paths_.game;
        options.game_running = [env, game]() { return AnyGameRunningIn(*env, game); };
        options.start_new = [this, &args](std::string& start_error) {
            LauncherLog(*env_, paths_, "update=installed; starting the new launcher");
            if (seams_.relaunch) return seams_.relaunch(paths_.launcher_exe, args, start_error);
            return StartSuccessor(paths_.launcher_exe, args, paths_.game, kSuccessorReadyWaitMs, start_error);
        };
        ApplyReport report;
        if (!ApplyUpdate(paths_, zip, target.latest_version, options, report, error)) {
            DeleteFileChecked(zip);
            LauncherLog(*env_, paths_, std::string("update=apply_failed; reason=") + ApplyFailureName(report.failure) + "; error=" + error +
                                           "; restored=" + (report.restored ? "yes" : "no"));
            fail(ApplyFailureSentence(report));
            return;
        }
        LauncherLog(*env_, paths_, "update=handed_over; version=" + target.latest_version + "; files=" + std::to_string(report.files));
        std::lock_guard<std::mutex> lock(update_mutex_);
        update_.status = UpdateStatus::ready_to_restart;
    }

    static const char* PhaseName(Phase p) {
        switch (p) {
        case Phase::not_in_game_folder: return "not_in_game_folder";
        case Phase::error: return "error";
        case Phase::ready: return "ready";
        case Phase::launching: return "launching";
        case Phase::in_game: return "in_game";
        case Phase::after_game: return "after_game";
        }
        return "?";
    }

    bool SessionPhase() const { return phase_ == Phase::launching || phase_ == Phase::in_game || phase_ == Phase::after_game; }

    void LoadChoices() {
        debug_ = DebugOptionsFromSettings(settings_);
        transport_ = settings_.Str("transport") == "udp" ? Transport::udp : Transport::steam;
        lan_.peer_ip = settings_.Str("udp_peer");
        const std::int64_t pl = settings_.Int("udp_port_local", kDefaultUdpPort);
        const std::int64_t pp = settings_.Int("udp_port_peer", kDefaultUdpPort);
        lan_.port_local = pl >= 1 && pl <= 65535 ? static_cast<int>(pl) : kDefaultUdpPort;
        lan_.port_peer = pp >= 1 && pp <= 65535 ? static_cast<int>(pp) : kDefaultUdpPort;
        name_ = settings_.Str("name");
        if (!name_.empty() && !IsValidPlayerName(name_)) name_.clear();
        // The stored picks: a hand-edited or no longer offered character, and the old default "sam"
        // from before Fragile (porter suit) became the guest's character, become kGuestCharacter
        // (GuestPickFromSettings). The marker goes into the next save.
        my_character_ = GuestPickFromSettings(settings_, settings_.Str("my_character"));
        if (const json::Value* t = settings_.Find("test_save"); t && t->is_object()) {
            test_folder_ = t->Str("folder");
            test_use_ = t->Bool("use", false);
            local_host_pad_ = t->Bool("host_pad", false);
            local_fps_ = LocalTestFps(static_cast<int>(t->Int("fps", kLocalTestFpsDefault)));
            local_guest_character_ = GuestPickFromSettings(settings_, t->Str("guest_character"));
        }
        if (settings_.Int("character_defaults", 0) < kCharacterDefaults) {
            // Migrated: the picks as read go back with the marker on the next save.
            settings_["my_character"] = my_character_;
            if (const json::Value* t = settings_.Find("test_save"); t && t->is_object())
                settings_["test_save"]["guest_character"] = local_guest_character_;
            settings_["character_defaults"] = kCharacterDefaults;
        }
        RefreshTestSave();
    }

    void SaveTestChoices() {
        json::Value t = json::Value::Object();
        t["folder"] = test_folder_;
        t["use"] = test_use_;
        t["host_pad"] = local_host_pad_;
        t["fps"] = local_fps_;
        t["guest_character"] = local_guest_character_;
        settings_["test_save"] = t;
        SaveChoices();
    }

    void RefreshTestSave() {
        test_check_ = test_folder_.empty() ? SeedCheck{} : CheckSeedFolder(Wide(test_folder_));
        test_seeded_ = SandboxSeeded(GetSandboxPaths(paths_, kTestSaveSandbox));
    }

    std::uint32_t Account() const {
        const std::uint64_t id = OwnSteamId(*env_);
        return id > 76561197960265728ull ? static_cast<std::uint32_t>(id - 76561197960265728ull) : 0u;
    }

    std::wstring SandboxRefusedPath() const { return JoinPath2(paths_.mod, L"sandbox-refused.txt"); }
    std::wstring SandboxActivePath() const { return JoinPath2(paths_.mod, L"sandbox-active.txt"); }
    // A test-save game must report its redirect (the mod writes sandbox-active.txt in DllMain, before
    // ds.exe's own code). Missing this long after the game started: the session.ini did not reach it.
    static constexpr std::int64_t kSandboxConfirmMs = 20000;

    // The test save for this session: seeded on first use, the real tree hashed before the game.
    bool PrepareSandbox(SessionIni& ini, const char* name, std::string& error) {
        if (!test_check_.ok) {
            error = test_folder_.empty() ? "Pick a test save folder first (Debug > Test saves)." : test_check_.note;
            return false;
        }
        const SandboxPaths sp = GetSandboxPaths(paths_, name);
        const std::wstring real = RealSaveRoot();
        if (!SandboxSeeded(sp) && !SeedSandbox(paths_, sp, &test_check_, real, Account(), test_folder_, error)) return false;
        ini.sandbox_saves = name;
        ini.sandbox_foreign_saves = true;   // a downloaded save belongs to another account; applied only after the redirect self-test
        real_before_ = SaveTreeManifest(real);
        std::string werr;
        WriteAtomic(JoinPath2(JoinPath2(paths_.mod, L"sandbox"), L"real-saves-before.txt"), real_before_, true, werr);
        DeleteFileChecked(SandboxRefusedPath());
        DeleteFileChecked(SandboxActivePath());   // the mod writes it again in DllMain of this game
        session_sandbox_ = true;
        sandbox_unconfirmed_ = false;
        sandbox_check_at_ = 0;
        test_seeded_ = true;
        LauncherLog(*env_, paths_, std::string("test_save=on sandbox=") + name);
        return true;
    }

    // The host-held save, guest (M1.2): the private coop-guest folder (its slots and incoming\ wiped, your own profile
    // and settings copied on first use), session.ini's three keys together, the real tree hashed before the game (the
    // same check as a test save after it).
    bool PrepareCoopGuest(SessionIni& ini, std::string& error) {
        const std::uint32_t account = Account();
        if (!account) {
            error = "Start Steam and log in first: the co-op save folder is made for your Steam account.";
            return false;
        }
        const std::wstring real = RealSaveRoot();
        SessionGuestReport report;
        if (!SeedSessionGuest(paths_, real, account, report, error)) return false;
        ini.host_save = true;
        ini.sandbox_saves = kCoopGuestSandbox;
        ini.sandbox_foreign_saves = true;   // the host's save belongs to the host's account; applied only after the redirect self-test
        real_before_ = SaveTreeManifest(real);
        std::string werr;
        WriteAtomic(JoinPath2(JoinPath2(paths_.mod, L"sandbox"), L"real-saves-before.txt"), real_before_, true, werr);
        DeleteFileChecked(SandboxRefusedPath());
        DeleteFileChecked(SandboxActivePath());   // the mod writes it again in DllMain of this game
        session_sandbox_ = true;
        sandbox_unconfirmed_ = false;
        sandbox_check_at_ = 0;
        LauncherLog(*env_, paths_, "host_save=guest sandbox=" + std::string(kCoopGuestSandbox) + " wiped_slots=" + std::to_string(report.wiped_slots) +
                                       " wiped_incoming=" + (report.wiped_incoming ? "1" : "0") + " removed_accounts=" +
                                       std::to_string(report.removed_accounts) + " copied=" + std::to_string(report.copied));
        return true;
    }

    // The host-held save, host (M1.2, M1.8): the optional backup (D1-2) and the hash of every file of the real tree that
    // is not a save slot, compared after the game (FinishHostCheck). The host's game itself runs on the real saves.
    bool PrepareHostSave(std::string& error) {
        host_check_ = false;
        const std::wstring real = RealSaveRoot();
        std::string backup = "off";
        if (debug_.backup_host_saves) {
            HostBackupReport report;
            const std::string stamp = FormatStamp(env_->LocalTime(env_->NowMs()));
            if (!BackupHostSaves(paths_, real, Account(), stamp, kHostBackupsKept, report, error)) {
                LauncherLog(*env_, paths_, "host_save=backup_failed");
                error += " Turn off \"Back up my saves before co-op\" on the Debug page to host without a backup.";
                return false;
            }
            backup = report.dir.empty() ? std::string("nothing_to_copy")
                                        : "ok slots=" + std::to_string(report.slots) + " files=" + std::to_string(report.files) +
                                              " pruned=" + std::to_string(report.pruned) +
                                              (report.newer_named ? " later_stamped_kept=" + std::to_string(report.newer_named) +
                                                                        " (the clock was set back: backups are ordered by their seq)"
                                                                  : std::string());
        }
        host_nonslot_before_ = NonSlotManifest(SaveTreeManifest(real));
        host_check_ = true;
        LauncherLog(*env_, paths_, "host_save=host backup=" + backup + " nonslot_files=" + std::to_string(SplitLines(host_nonslot_before_).size()));
        return true;
    }

    // After a hosted host-save game: every non-slot file of the real tree compared (launcher.log only; the game itself
    // may write its profile and settings, the mod writes nothing there).
    void FinishHostCheck() {
        if (!host_check_) return;
        host_check_ = false;
        const std::vector<std::string> changes = ManifestChanges(host_nonslot_before_, NonSlotManifest(SaveTreeManifest(RealSaveRoot())));
        std::string first;
        if (!changes.empty()) {
            // Account folders (all digits) never go into the log.
            std::string c = changes[0];
            std::string masked;
            for (std::size_t i = 0; i < c.size();) {
                std::size_t j = i;
                while (j < c.size() && c[j] >= '0' && c[j] <= '9') ++j;
                if (j > i && j - i >= 5) masked += "<id>", i = j;
                else masked += c[i++];
            }
            first = " first=" + masked;
        }
        LauncherLog(*env_, paths_, "host_save=checked nonslot_changed=" + std::to_string(changes.size()) + first);
    }

    // After a game on a sandbox: did it refuse, and is the real save tree what it was?
    // local: the local two-player test (its games' refusals are in instances\host|guest; a
    // "Play with test save" game's in the mod folder: never another mode's leftover file).
    std::string CheckRealSaves(TestSaveView::Tone& tone, bool local) {
        std::string text;
        if (local ? ReadLocalRefusal(text) : ReadTextShared(SandboxRefusedPath(), text, 1u << 16)) {
            std::string reason;
            for (const std::string& l : SplitLines(text))
                if (StartsWith(l, "reason=")) reason = l.substr(7);
            tone = TestSaveView::Tone::bad;
            return "The game refused to start with the test save, so it never touched any save: " +
                   (reason.empty() ? std::string("the save redirect could not be installed.") : reason);
        }
        const std::vector<std::string> changes = ManifestChanges(real_before_, SaveTreeManifest(RealSaveRoot()));
        if (!local && sandbox_unconfirmed_) {
            tone = TestSaveView::Tone::bad;
            return "The game never confirmed the test save (the mod wrote no sandbox-active.txt), so it may have run on your own saves" +
                   (changes.empty() ? std::string(" (your save folder is unchanged).")
                                    : " (" + std::to_string(changes.size()) + " file(s) changed in your save folder, e.g. " + changes[0] + ").");
        }
        if (changes.empty()) {
            tone = TestSaveView::Tone::good;
            return "Your own saves were not changed (every file checked by SHA-256 before and after).";
        }
        tone = TestSaveView::Tone::warn;
        LauncherLog(*env_, paths_, "test_save=real_tree_changed count=" + std::to_string(changes.size()));
        return "Your save folder changed while the test save ran (" + std::to_string(changes.size()) + " file(s), e.g. " + changes[0] +
               "). The game itself only wrote to the test copy; Steam Cloud may have synced. Check before you play normally.";
    }

    bool ReadLocalRefusal(std::string& text) {
        for (const wchar_t* who : {L"host", L"guest"})
            if (ReadTextShared(JoinPath2(LocalDir(who), L"sandbox-refused.txt"), text, 1u << 16)) return true;
        return false;
    }

    void FinishSandboxCheck() {
        if (!session_sandbox_) return;
        session_sandbox_ = false;
        sandbox_check_at_ = 0;
        test_result_ = CheckRealSaves(test_tone_, false);
        sandbox_unconfirmed_ = false;
        LauncherLog(*env_, paths_, std::string("test_save=checked tone=") + std::to_string(static_cast<int>(test_tone_)));
    }

    // ---- local two-instance test --------------------------------------------------------
    static constexpr std::int64_t kLocalGuestDelayMs = 8000;
    struct LocalTest {
        bool running = false;
        bool guest_started = false;
        std::uint32_t host_pid = 0, guest_pid = 0;
        std::int64_t started_ms = 0, guest_due_ms = 0, gone_since = 0, next_status_ms = 0;
        std::string host_state, guest_state, headline, detail, features_off;
        bool host_save = false;   // the two games run the host-held save session
        LinkView::Tone tone = LinkView::Tone::neutral;
    };

    std::wstring LocalDir(const wchar_t* who) const { return JoinPath2(JoinPath2(paths_.mod, L"instances"), who); }
    static std::vector<std::wstring> LocalEnv(const wchar_t* who) {
        // SteamAppId: steam_api64.dll's RestartAppIfNecessary answers false (no relaunch through
        // Steam); ST_INSTANCE_DIR: the mod's per-game folder (session.ini, log, link status).
        return {L"SteamAppId=1850570", L"SteamGameId=1850570", std::wstring(L"ST_INSTANCE_DIR=instances\\") + who};
    }
    void CleanupLocalIni() {
        for (const wchar_t* who : {L"host", L"guest"}) DeleteFileChecked(JoinPath2(LocalDir(who), L"session.ini"));
    }
    std::string LocalState(const wchar_t* who, std::uint32_t pid, std::string* features_off = nullptr) {
        std::string text;
        if (!ReadTextShared(JoinPath2(LocalDir(who), L"link-status.json"), text, 1u << 20)) return {};
        const json::Value st = ParseStatusText(text, pid);
        if (features_off && st.is_object()) *features_off = st.Str("features_off");
        return st.is_object() ? st.Str("state") : std::string();
    }

    void TickLocalTest(std::int64_t now) {
        if (!local_.running) return;
        const bool host_alive = local_.host_pid && env_->ProcessAlive(local_.host_pid);
        if (!local_.guest_started && now >= local_.guest_due_ms) {
            if (host_alive) {
                std::uint32_t pid = 0;
                local_.guest_started = true;
                if (env_->StartProcess(paths_.exe, paths_.game, LocalEnv(L"guest"), pid) && pid) {
                    local_.guest_pid = pid;
                    LauncherLog(*env_, paths_, "local_test=guest_started pid=" + std::to_string(pid));
                } else {
                    local_.detail = "The second game could not be started.";
                    LauncherLog(*env_, paths_, "local_test=guest_start_failed");
                }
            } else {
                local_.guest_started = true;   // nothing to start next to
            }
        }
        const bool guest_alive = local_.guest_pid && env_->ProcessAlive(local_.guest_pid);
        if (now >= local_.next_status_ms) {
            local_.next_status_ms = now + 1000;
            std::string host_off, guest_off;
            local_.host_state = host_alive ? LocalState(L"host", local_.host_pid, &host_off) : std::string();
            local_.guest_state = guest_alive ? LocalState(L"guest", local_.guest_pid, &guest_off) : std::string();
            local_.features_off = (host_off.empty() ? std::string() : "Host game: " + host_off) +
                                  (!host_off.empty() && !guest_off.empty() ? "\n" : "") +
                                  (guest_off.empty() ? std::string() : "Guest game: " + guest_off);
        }
        if (host_alive || guest_alive || !local_.guest_started) {
            local_.gone_since = 0;
            const bool both = host_alive && guest_alive;
            const bool linked = local_.host_state == "connected" && local_.guest_state == "connected";
            local_.headline = linked ? "Both games are linked" : both ? "Both games are running" : host_alive ? "The first game (host) runs" : "The second game (guest) runs";
            local_.detail = linked ? "Each window shows the other player. Keyboard + mouse play the host window, a pad plays the guest."
                            : local_.host_save
                                ? "Continue (or New Game) in the host window; the guest window waits for the host and then loads the host's save. "
                                  "The second game may first say it cannot reach the game server: press OK."
                                : "Load a save in each window (Continue). The second game may first say it cannot reach the game server: press OK.";
            local_.tone = linked ? LinkView::Tone::good : LinkView::Tone::neutral;
            return;
        }
        if (!local_.gone_since) local_.gone_since = now;
        if (now - local_.gone_since < 3000) return;
        // Both games are gone: unread one-shot files go, the real save tree is compared.
        CleanupLocalIni();
        TestSaveView::Tone tone = TestSaveView::Tone::neutral;
        test_result_ = CheckRealSaves(tone, true);
        test_tone_ = tone;
        local_.running = false;
        local_.headline = "The local test has ended";
        local_.detail = test_result_;
        local_.tone = tone == TestSaveView::Tone::good ? LinkView::Tone::good : tone == TestSaveView::Tone::bad ? LinkView::Tone::bad : LinkView::Tone::warn;
        LauncherLog(*env_, paths_, "local_test=ended");
    }

    void SaveChoices() {
        if (!IsDir(paths_.mod) && !checks_.mod_present) return;   // nothing of ours in the folder yet
        std::string error;
        SaveSettings(paths_, settings_, error);
    }

    void Recheck() {
        last_error_.clear();
        discovered_.clear();
        checks_ = InstallChecks{};
        if (!IsFile(paths_.exe)) {
            phase_ = Phase::not_in_game_folder;
            for (const std::wstring& f : FindGameFolders(*env_))
                if (!IEquals(f, paths_.game)) {
                    discovered_ = Utf8(f);
                    break;
                }
            return;
        }
        gate_ = CheckGate(*env_, paths_, settings_);
        mod_sig_ = ModSignature();
        package_ = ReadPackage(paths_);
        const ModCheck mod = CheckMod(paths_, package_, version());
        checks_.game_found = gate_.game_found;
        checks_.version_note = UntestedVersionText(gate_);
        checks_.version_tested = checks_.version_note.empty();
        checks_.mod_state = mod.state;
        checks_.mod_note = mod.note;
        checks_.package_version = package_.ok ? package_.version : std::string();
        checks_.mod_present = mod.state == ModState::ok || mod.state == ModState::dev_build || mod.state == ModState::mismatch;
        checks_.steam_running = SteamClientRunning(*env_);
        SaveChoices();   // keeps the hash cache
        if (mod.state != logged_mod_state_) {
            logged_mod_state_ = mod.state;
            LauncherLog(*env_, paths_, std::string("mod_state=") + ModStateName(mod.state) +
                                           (package_.ok ? " package=" + package_.build_id : std::string()));
        }
        if (!gate_.path_problem.empty()) {
            phase_ = Phase::error;
            last_error_ = gate_.path_problem;
            return;
        }
        if (!checks_.mod_present) {
            phase_ = Phase::error;
            last_error_ = mod.state == ModState::foreign    ? mod.note
                          : mod.state == ModState::disabled ? ModDisabledText()
                                                            : ModMissingText();
            return;
        }
        if (SessionPhase()) return;   // a running session keeps its phase
        phase_ = Phase::ready;
        AdoptSession();
    }

    // A session this window did not start (a window closed and opened again,
    // or another window): follow it, or tidy a leftover one.
    void AdoptSession() {
        json::Value r;
        const bool active = ReadSessionRecord(paths_, r) && RecordIsActive(r);
        const bool game = AnyGameRunningIn(*env_, paths_.game);
        if (active && (game || SessionOwnerLive(*env_, r))) {
            session_id_ = r.Str("session_id");
            role_ = RoleFromIni(r.Str("role"));
            session_transport_ = r.Str("transport") == "udp" ? Transport::udp : Transport::steam;
            code_.clear();
            if (role_ == Role::host) {
                std::string text, error;
                SessionIni ini;
                // A one-shot session.ini is renamed to session.last.ini by the mod once the game has read it.
                const std::string recorded = r.Str("ini_sha256");
                for (const std::wstring* path : {&paths_.session_ini, &paths_.session_last_ini}) {
                    if (!IsFile(*path) || !ReadTextShared(*path, text, 1u << 20)) continue;
                    if (!recorded.empty() && Sha256Hex(text) != recorded) continue;
                    if (ParseSessionIniText(text, ini, error)) code_ = FormatJoinCode(ini.secret, OwnSteamId(*env_));
                    break;
                }
            }
            phase_ = game ? Phase::in_game : Phase::launching;
            launch_ms_ = env_->NowMs();
            gone_since_ = 0;
            LauncherLog(*env_, paths_, std::string("session=adopted phase=") + PhaseName(phase_));
            return;
        }
        if (!game && StaleSessionPending(*env_, paths_)) {
            const CleanupResult c = CleanupSession(*env_, paths_);
            LauncherLog(*env_, paths_, std::string("session=stale_cleanup ran=") + (c.ran ? "1" : "0"));
        }
    }

    // Downloading or installing an update: the game folder's files are about to change
    // (and the update refuses to install while ds.exe runs), so nothing starts the game
    // or switches the mod meanwhile.
    bool UpdateRunning() const {
        std::lock_guard<std::mutex> lock(update_mutex_);
        return update_.status == UpdateStatus::downloading || update_.status == UpdateStatus::applying ||
               update_.status == UpdateStatus::ready_to_restart;
    }
    static constexpr char kUpdateRunningText[] = "An update is being installed. Wait until the launcher restarts, then try again.";

    bool ReadyFor(std::string& error) {
        if (UpdateRunning()) {
            error = kUpdateRunningText;
            return false;
        }
        switch (phase_) {
        case Phase::ready: break;
        case Phase::launching:
        case Phase::in_game:
        case Phase::after_game: error = "A game is already running. Close it first."; return false;
        default: error = last_error_.empty() ? "Co-op can't start from this folder right now." : last_error_; return false;
        }
        if (local_.running) {
            error = "The local two-player test is still running. Close both test games first.";
            return false;
        }
        if (AnyGameRunningIn(*env_, paths_.game)) {
            error = "DEATH STRANDING is already running. Close it first, then start co-op from here so the mod gets its settings.";
            return false;
        }
        return true;
    }

    SessionIni BaseIni(const char* role) const {
        SessionIni ini;
        ini.role = role;
        ini.name = name_;
        // The host is always Sam; every other role carries the Join tab's choice (the writer
        // puts sam for a host anyway).
        ini.my_character = std::string(role) == "host" ? std::string(kHostCharacter) : my_character();
        ApplyDebugOptions(debug_, ini);
        return ini;
    }

    static void Loopback(SessionIni& ini) {
        ini.transport = "udp";
        ini.udp_peer = "127.0.0.1";
        ini.udp_port_local = kDefaultUdpPort;
        ini.udp_port_peer = kDefaultUdpPort + 1;
    }

    std::string ModSignature() const {
        return FileStamp(paths_.mod_dll) + "|" + FileStamp(paths_.mod_dll_disabled) + "|" + FileStamp(paths_.package_json) + "|" +
               FileStamp(paths_.launcher_exe);
    }

    bool ApplyTransport(SessionIni& ini, std::string& error) const {
        if (transport_ == Transport::steam) {
            ini.transport = "steam";
            return true;
        }
        ini.transport = "udp";
        if (!IsCanonicalIPv4(lan_.peer_ip)) {
            error = "LAN mode: enter your partner's IPv4 address (for example 192.168.1.20) under Advanced.";
            return false;
        }
        if (lan_.port_local < 1 || lan_.port_local > 65535 || lan_.port_peer < 1 || lan_.port_peer > 65535) {
            error = "LAN mode: the ports must be numbers from 1 to 65535.";
            return false;
        }
        ini.udp_peer = lan_.peer_ip;
        ini.udp_port_local = lan_.port_local;
        ini.udp_port_peer = lan_.port_peer;
        return true;
    }

    bool StartSession(SessionIni ini, Role role, std::string& error) {
        if (ini.sandbox_saves.empty()) {
            session_sandbox_ = false;
            if (test_use_ && !PrepareSandbox(ini, kTestSaveSandbox, error)) return false;
        }
        host_check_ = false;
        // A host-save host on its real saves (not on a test save): backup and the non-slot hash first.
        if (ini.role == "host" && ini.host_save && ini.sandbox_saves.empty() && !PrepareHostSave(error)) return false;
        std::string id;
        if (!ActivateSession(*env_, paths_, ini, id, error)) return false;
        if (!StartWatchdogFor(*env_, paths_, id))
            LauncherLog(*env_, paths_, "watchdog=unavailable; the window tidies up while it stays open");
        if (!env_->OpenUrl(Wide(kSteamUrl))) {
            CleanupSession(*env_, paths_);
            error = "Steam could not be started. Start Steam, log in, then try again.";
            return false;
        }
        LauncherLog(*env_, paths_, "launch=steam role=" + ini.role + " transport=" + ini.transport + " my_character=" + ini.my_character +
                                       " partner_body=" + ini.partner_body + " host_save=" + (ini.host_save ? "1" : "0"));
        session_id_ = id;
        role_ = role;
        session_transport_ = ini.transport == "udp" ? Transport::udp : Transport::steam;
        phase_ = Phase::launching;
        launch_ms_ = env_->NowMs();
        gone_since_ = 0;
        pending_ = true;
        record_lost_ = false;
        last_error_.clear();
        link_ = LinkView{};
        link_.headline = "Starting DEATH STRANDING...";
        link_.detail = "Steam is starting the game. This can take a minute.";
        return true;
    }

    void EndSessionState() {
        session_id_.clear();
        code_.clear();
        gone_since_ = 0;
        link_ = LinkView{};
    }

    void PollStatus(std::int64_t now) {
        if (now < next_status_ms_) return;
        next_status_ms_ = now + kStatusEveryMs;
        std::string text;
        json::Value status;
        if (IsFile(paths_.link_status) && ReadTextShared(paths_.link_status, text, 1u << 20)) status = ParseStatusText(text, 0);
        bool fresh = false;
        if (status.is_object()) {
            const std::int64_t pid = status.Int("pid", 0);
            fresh = pid > 0 && env_->ProcessAlive(static_cast<std::uint32_t>(pid)) && StatusTimeFresh(status, now);
        }
        // While the game starts, an old or foreign status means nothing yet.
        if (phase_ == Phase::launching && !fresh) status = json::Value();
        if (phase_ == Phase::launching && !status.is_object()) {
            link_ = LinkView{};
            link_.headline = "Starting DEATH STRANDING...";
            link_.detail = "Steam is starting the game. This can take a minute.";
            return;
        }
        // In game without any status for a while: the mod did not load.
        const StatusView v = StatusViewOf(status, fresh, RoleIniName(role_));
        LinkView l;
        l.state = v.state;
        l.fresh = v.fresh;
        l.connected = v.connected;
        l.ping_ms = v.rtt_ms;
        l.transport = v.transport;
        l.route = v.route;
        l.peer = v.peer;
        l.remote = v.remote;
        l.headline = v.headline;
        l.detail = v.detail;
        l.tone = ToneOf(v.tone);
        l.features_off = v.fresh ? v.features_off : std::string();
        if (!status.is_object() && phase_ == Phase::in_game && now - launch_ms_ > 120000) {
            l.headline = "The mod has not reported in";
            l.detail = "The game runs, but co-op has not written its status. Check that winhttp.dll is the Stranded Together one, "
                       "then Collect logs from Tools.";
            l.tone = LinkView::Tone::warn;
        }
        if (l.state != link_.state) LauncherLog(*env_, paths_, "link state=" + (l.state.empty() ? std::string("none") : l.state));
        link_ = l;
    }

    std::shared_ptr<Env> env_;
    InstancePaths paths_;
    json::Value settings_;
    GateResult gate_;
    PackageInfo package_;
    std::string mod_sig_;
    ModState logged_mod_state_ = static_cast<ModState>(-1);
    DebugOptions debug_;
    bool crash_present_ = false;
    Phase phase_ = Phase::ready;
    InstallChecks checks_;
    std::string last_error_, discovered_;
    Transport transport_ = Transport::steam;
    LanSettings lan_;
    std::string name_;

    std::string session_id_, code_;
    Role role_ = Role::host;
    // Test saves and the local test.
    std::string test_folder_;
    SeedCheck test_check_;
    bool test_use_ = false, test_seeded_ = false, local_host_pad_ = false;
    int local_fps_ = kLocalTestFpsDefault;                   // local test: "test_save" / "fps" (LocalTestFps)
    std::string my_character_ = kGuestCharacter;            // Join tab: "my_character" (the stored pick)
    std::string local_guest_character_ = kGuestCharacter;   // local test: "test_save" / "guest_character"
    bool session_sandbox_ = false;   // the session started from here runs on a sandbox
    bool sandbox_unconfirmed_ = false;   // that game never wrote sandbox-active.txt
    std::int64_t sandbox_check_at_ = 0;  // when to look for sandbox-active.txt (0 = not pending)
    std::string real_before_;        // the real save tree's manifest before that game
    bool host_check_ = false;        // a host-save host on the real saves: compare host_nonslot_before_ after the game
    std::string host_nonslot_before_;
    std::string test_result_;
    TestSaveView::Tone test_tone_ = TestSaveView::Tone::neutral;
    LocalTest local_;
    Transport session_transport_ = Transport::steam;
    std::int64_t launch_ms_ = 0, gone_since_ = 0, after_until_ = 0;
    std::int64_t next_check_ms_ = 0, next_status_ms_ = 0;
    bool pending_ = false;
    bool record_lost_ = false;   // a session phase, but its record was ended elsewhere (helper timeout, End session)
    LinkView link_;

    // Updates. update_ and auto_check_ are shared with the update thread (update_mutex_).
    ModelStart start_;
    UpdateSeams seams_;
    std::once_flag http_once_;
    mutable std::mutex update_mutex_;
    UpdateInfo update_;
    bool auto_check_ = true;
    std::string updated_to_;
    bool auto_checked_ = false;
    bool update_now_started_ = false;
    std::atomic<bool> update_cancel_{false};
    std::thread update_thread_;
};

}  // namespace

std::unique_ptr<LauncherModel> MakeRealModelWithEnv(const std::wstring& exe_dir, std::shared_ptr<Env> env, const ModelStart& start,
                                                    UpdateSeams seams) {
    return std::make_unique<RealModel>(exe_dir, std::move(env), start, std::move(seams));
}

int RunWatch(const std::wstring& game_dir, const std::string& session_id) {
    std::shared_ptr<Env> env = MakeWin32Env();
    return RunWatchdog(*env, game_dir, session_id);
}

int RunWriteSession(const std::wstring& game_dir, const WriteSessionRequest& request, std::string& report) {
    std::shared_ptr<Env> env = MakeWin32Env();
    const InstancePaths paths = GetInstancePaths(game_dir);
    auto fail = [&](const std::string& why) {
        report += "result=failed\nerror=" + why + "\n";
        return 1;
    };
    if (!IsFile(paths.exe)) return fail("no ds.exe in " + Utf8(paths.game));
    SessionIni ini;
    json::Value settings;
    ReadSettings(paths, settings);
    ApplyDebugOptions(DebugOptionsFromSettings(settings), ini);   // the Debug page's options, one_shot=1
    // The Join tab's choice as the launcher would write it (a host writes sam).
    ini.my_character = GuestCharacter(GuestPickFromSettings(settings, settings.Str("my_character")),
                                      DebugOptionsFromSettings(settings).allow_other_characters);
    ini.transport = request.transport.empty() ? "steam" : request.transport;
    if (ini.transport == "udp") {
        ini.udp_peer = request.udp_peer.empty() ? std::string("127.0.0.1") : request.udp_peer;
        ini.udp_port_local = request.udp_port_local ? request.udp_port_local : kDefaultUdpPort;
        ini.udp_port_peer = request.udp_port_peer ? request.udp_port_peer : kDefaultUdpPort;
    }
    std::string code;
    const bool host_save = DebugOptionsFromSettings(settings).host_save;
    ini.world_sync = DebugOptionsFromSettings(settings).share_world;   // written with host_save (world_sync=0 when off)
    const std::uint64_t self_id = OwnSteamId(*env);
    const std::uint32_t account = self_id > kSteamIdBase ? static_cast<std::uint32_t>(self_id - kSteamIdBase) : 0u;
    if (request.role == "host") {
        ini.role = "host";
        ini.secret = NewSecret();
        if (!self_id) return fail("Steam is not logged in (no ActiveUser): a host code needs your SteamID64");
        code = FormatJoinCode(ini.secret, self_id);
        ini.host_save = host_save;   // no backup here (developer tool); the Host button makes one
    } else if (request.role == "join") {
        ini.role = "guest";
        std::uint64_t host = 0;
        if (!ParseJoinCode(request.code, ini.secret, host)) return fail("--code is not a valid DST1 join code");
        ini.peer_steamid = Decimal(host);
        if (host_save) {
            // As Join does: the private coop-guest folder, never the real saves.
            SessionGuestReport seeded;
            std::string error;
            if (!SeedSessionGuest(paths, RealSaveRoot(), account, seeded, error)) return fail(error);
            ini.host_save = true;
            ini.sandbox_saves = kCoopGuestSandbox;
            ini.sandbox_foreign_saves = true;
        }
    } else if (request.role == "echo" || request.role == "solo") {
        ini.role = request.role;
        if (request.role == "solo") ini.debug_menu = true;
        if (request.transport.empty()) {
            ini.transport = "udp";
            ini.udp_peer = "127.0.0.1";
            ini.udp_port_local = kDefaultUdpPort;
            ini.udp_port_peer = kDefaultUdpPort + 1;
        }
    } else {
        return fail("role must be host, join, echo or solo");
    }
    std::string id, error;
    if (!ActivateSession(*env, paths, ini, id, error)) return fail(error);
    report += "result=ok\nrole=" + ini.role + "\ntransport=" + ini.transport + "\nhost_save=" + (ini.host_save ? "1" : "0") +
              "\nsession_ini=" + Utf8(paths.session_ini) + "\n";
    if (!code.empty()) report += "code=" + code + "\n";
    report += "note=one_shot: the mod renames session.ini to session.last.ini when the game reads it. No helper runs: "
              "--end-session (or the next launcher start with the game closed) removes an unread session.ini\n";
    return 0;
}

int RunWriteLocalTestIni(const std::wstring& game_dir, const std::wstring& out_dir, std::string& report) {
    auto fail = [&](const std::string& why) {
        report += "result=failed\nerror=" + why + "\n";
        return 1;
    };
    if (out_dir.empty()) return fail("--out <folder> is needed (nothing is written into the game folder)");
    // The Debug page's options and the local test's picks as the launcher in that folder has them (read only).
    json::Value settings;
    if (!game_dir.empty()) {
        const InstancePaths paths = GetInstancePaths(game_dir);
        ReadSettings(paths, settings);
    }
    const DebugOptions options = DebugOptionsFromSettings(settings);
    LocalTestChoices choices;
    choices.name = settings.Str("name");
    if (!choices.name.empty() && !IsValidPlayerName(choices.name)) choices.name.clear();
    std::string stored_pick;
    if (const json::Value* t = settings.Find("test_save"); t && t->is_object()) {
        stored_pick = t->Str("guest_character");
        choices.host_pad = t->Bool("host_pad", false);
        choices.fps = LocalTestFps(static_cast<int>(t->Int("fps", kLocalTestFpsDefault)));
    }
    choices.guest_character = GuestPickFromSettings(settings, stored_pick);
    SessionIni host, guest;
    MakeLocalTestInis(options, choices, host, guest);
    if (!CreateDirectories(out_dir)) return fail("could not create " + Utf8(out_dir));
    for (const auto& [ini, file] : {std::pair<const SessionIni*, const wchar_t*>{&host, L"host.ini"}, {&guest, L"guest.ini"}}) {
        std::string text, error, werr;
        if (!MakeSessionIniText(*ini, text, error)) return fail(std::string(file == std::wstring(L"host.ini") ? "host" : "guest") + ": " + error);
        if (!WriteAtomic(JoinPath2(out_dir, file), text, true, werr)) return fail("could not write " + Utf8(JoinPath2(out_dir, file)) + ": " + werr);
    }
    report += "result=ok\nhost_save=" + std::string(host.host_save ? "1" : "0") + "\ngame_fps=" + std::to_string(LocalTestGameFps(choices.fps)) + "\nhost_ini=" + Utf8(JoinPath2(out_dir, L"host.ini")) +
              "\nguest_ini=" + Utf8(JoinPath2(out_dir, L"guest.ini")) + "\n";
    return 0;
}

int RunEndSession(const std::wstring& game_dir, std::string& report) {
    std::shared_ptr<Env> env = MakeWin32Env();
    const InstancePaths paths = GetInstancePaths(game_dir);
    const CleanupResult r = CleanupSession(*env, paths);
    report += std::string("result=") + (r.ran && !r.ini_kept ? "ok" : "failed") + "\n";
    report += std::string("session_ini_removed=") + (r.ini_removed ? "1" : "0") + "\n";
    if (!r.message.empty()) report += "message=" + r.message + "\n";
    return r.ran && !r.ini_kept ? 0 : 1;
}

}  // namespace st::core

namespace st {
std::unique_ptr<LauncherModel> MakeRealModel(const std::wstring& exe_dir, const ModelStart& start) {
    return core::MakeRealModelWithEnv(exe_dir, core::MakeWin32Env(), start);
}
}  // namespace st
