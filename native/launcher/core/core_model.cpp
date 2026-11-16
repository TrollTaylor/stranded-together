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
#include "core_session.hpp"
#include "core_text.hpp"
#include "core_update.hpp"
#include "launcher_join_code.hpp"

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
    const int key = ParseFunctionKey(d->Str("debug_menu_key"));
    if (key) o.debug_menu_key = key;
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
    d["debug_menu_key"] = FunctionKeyName(o.debug_menu_key);
    settings["debug"] = d;
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
                   " debug_menu=" + (debug_.debug_menu ? "1" : "0") + " key=" + FunctionKeyName(debug_.debug_menu_key) + "\n";
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
        switch (phase_) {
        case Phase::launching: {
            if (AnyGameRunningIn(*env_, paths_.game)) {
                LauncherLog(*env_, paths_, "game=started");
                phase_ = Phase::in_game;
                gone_since_ = 0;
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

    bool StartSession(const SessionIni& ini, Role role, std::string& error) {
        std::string id;
        if (!ActivateSession(*env_, paths_, ini, id, error)) return false;
        if (!StartWatchdogFor(*env_, paths_, id))
            LauncherLog(*env_, paths_, "watchdog=unavailable; the window tidies up while it stays open");
        if (!env_->OpenUrl(Wide(kSteamUrl))) {
            CleanupSession(*env_, paths_);
            error = "Steam could not be started. Start Steam, log in, then try again.";
            return false;
        }
        LauncherLog(*env_, paths_, "launch=steam role=" + ini.role + " transport=" + ini.transport);
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
    ini.transport = request.transport.empty() ? "steam" : request.transport;
    if (ini.transport == "udp") {
        ini.udp_peer = request.udp_peer.empty() ? std::string("127.0.0.1") : request.udp_peer;
        ini.udp_port_local = request.udp_port_local ? request.udp_port_local : kDefaultUdpPort;
        ini.udp_port_peer = request.udp_port_peer ? request.udp_port_peer : kDefaultUdpPort;
    }
    std::string code;
    if (request.role == "host") {
        ini.role = "host";
        ini.secret = NewSecret();
        const std::uint64_t self = OwnSteamId(*env);
        if (!self) return fail("Steam is not logged in (no ActiveUser): a host code needs your SteamID64");
        code = FormatJoinCode(ini.secret, self);
    } else if (request.role == "join") {
        ini.role = "guest";
        std::uint64_t host = 0;
        if (!ParseJoinCode(request.code, ini.secret, host)) return fail("--code is not a valid DST1 join code");
        ini.peer_steamid = Decimal(host);
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
    report += "result=ok\nrole=" + ini.role + "\ntransport=" + ini.transport + "\nsession_ini=" + Utf8(paths.session_ini) + "\n";
    if (!code.empty()) report += "code=" + code + "\n";
    report += "note=one_shot: the mod renames session.ini to session.last.ini when the game reads it. No helper runs: "
              "--end-session (or the next launcher start with the game closed) removes an unread session.ini\n";
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
