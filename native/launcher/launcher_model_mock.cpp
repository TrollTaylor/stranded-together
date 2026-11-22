// Stranded Together: the simulated LauncherModel (see launcher_model_mock.hpp).
#include "launcher_model_mock.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <random>

#include "core/core_text.hpp"
#include "launcher_join_code.hpp"
#include "launcher_ui_logic.hpp"
#include "st_launcher_version.h"

namespace st {
namespace {
constexpr double kLaunch = 4.0;
constexpr double kConnectAt = 5.0;
constexpr double kVisibleAt = 8.0;
constexpr double kDropAt = 34.0;
constexpr double kGameLength = 90.0;
constexpr double kAfterGame = 4.0;

const char kSteamDir[] = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DEATH STRANDING DIRECTORS CUT";
const char kDownloadsDir[] = "C:\\Users\\Sam\\Downloads\\StrandedTogether";

std::string DesktopFile(const std::string& name) {
    char* profile = nullptr;
    std::size_t size = 0;
    std::string dir = "C:\\Users\\Sam";
    if (_dupenv_s(&profile, &size, "USERPROFILE") == 0 && profile && *profile) dir = profile;
    std::free(profile);
    return dir + "\\Desktop\\" + name;
}

std::string Stamp(bool fixed) {
    if (fixed) return "20261115-153000";
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char buf[32]{};
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &local);
    return buf;
}

LinkView FromStatusView(const core::StatusView& v) {
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
    switch (v.tone) {
    case core::StatusView::Tone::good: l.tone = LinkView::Tone::good; break;
    case core::StatusView::Tone::warn: l.tone = LinkView::Tone::warn; break;
    case core::StatusView::Tone::bad: l.tone = LinkView::Tone::bad; break;
    case core::StatusView::Tone::neutral: break;
    }
    return l;
}
}  // namespace

bool ParseMockScenario(const std::string& name, MockScenario& out) {
    static const struct {
        const char* name;
        MockScenario scenario;
    } kNames[] = {
        {"ready", MockScenario::ready},
        {"wrong-folder", MockScenario::wrong_folder},
        {"not-installed", MockScenario::not_installed},
        {"untested-version", MockScenario::untested_version},
        {"steam-offline", MockScenario::steam_offline},
        {"partner-drops", MockScenario::partner_drops},
        {"dev-build", MockScenario::dev_build},
        {"mod-mismatch", MockScenario::mod_mismatch},
        {"mod-foreign", MockScenario::mod_foreign},
        {"mod-disabled", MockScenario::mod_disabled},
        {"update-available", MockScenario::update_available},
    };
    for (const auto& n : kNames)
        if (name == n.name) {
            out = n.scenario;
            return true;
        }
    return false;
}

MockModel::MockModel(const MockOptions& options) : options_(options) {
    if (!(options_.speed > 0.0)) options_.speed = 1.0;
    epoch_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    lan_.peer_ip = "192.168.1.20";
    update_.current_version = ST_VERSION_STRING;
    set_scenario(options_.scenario);
    if (options_.scenario == MockScenario::update_available) update_ = MockRelease();
}

UpdateInfo MockModel::MockRelease() {
    UpdateInfo u;
    u.status = UpdateStatus::available;
    u.current_version = ST_VERSION_STRING;
    u.latest_version = "0.2.0";
    u.tag = "v0.2.0";
    u.release_title = "Stranded Together 0.2.0";
    u.release_notes = "## What's new\n\n- The partner now walks and runs with Sam's animations instead of gliding.\n"
                      "- Cargo your partner carries shows on their back.\n- Fixed: the join code was sometimes not copied.\n";
    u.html_url = "https://github.com/TrollTaylor/stranded-together/releases/tag/v0.2.0";
    u.asset_name = "StrandedTogether-0.2.0.zip";
    u.download_url = "https://github.com/TrollTaylor/stranded-together/releases/download/v0.2.0/StrandedTogether-0.2.0.zip";
    u.download_size = 1835008;
    return u;
}

UpdateInfo MockModel::update_info() const { return update_; }

void MockModel::check_for_updates() {
    if (update_.status == UpdateStatus::checking || update_.status == UpdateStatus::downloading || update_.status == UpdateStatus::applying)
        return;
    update_.status = UpdateStatus::checking;
    update_.error.clear();
    update_started_ = now();
}

void MockModel::start_update() {
    if (update_.status != UpdateStatus::available && !(update_.status == UpdateStatus::error && !update_.error_after_check)) return;
    if (ui::SessionActive(phase_)) {
        update_.status = UpdateStatus::error;
        update_.error = "Quit DEATH STRANDING first: the update replaces files the game uses. Then press Update now again.";
        update_.error_after_check = false;
        return;
    }
    update_.status = UpdateStatus::downloading;
    update_.bytes_downloaded = 0;
    update_.progress = 0.0f;
    update_.error.clear();
    update_started_ = now();
}

void MockModel::set_scenario(MockScenario scenario) {
    options_.scenario = scenario;
    last_error_.clear();
    link_override_ = LinkOverride::none;
    switch (scenario) {
    case MockScenario::wrong_folder: phase_ = Phase::not_in_game_folder; break;
    case MockScenario::not_installed:
        phase_ = Phase::error;
        last_error_ = "The co-op mod is not installed in this game folder. Extract everything from the Stranded Together zip into "
                      "the game folder, so winhttp.dll sits next to ds.exe, then press Check again.";
        break;
    case MockScenario::mod_foreign:
        phase_ = Phase::error;
        last_error_ = "There is a winhttp.dll in the game folder that is not Stranded Together (another mod or tool uses the same file "
                      "name). Stranded Together never replaces it. Move it out of the game folder yourself, extract the Stranded "
                      "Together zip again, then press Check again.";
        break;
    case MockScenario::mod_disabled:
        phase_ = Phase::error;
        last_error_ = "The co-op mod is switched off (winhttp.dll.disabled), so the game runs unmodded. Press Enable mod to switch it "
                      "back on.";
        break;
    default: phase_ = Phase::ready; break;
    }
}

std::string MockModel::PartnerCode() { return FormatJoinCode("7QK2MX9RT4PB8WZ3HCNA", kPartnerSteamId); }

double MockModel::now() const {
    if (manual_clock_) return manual_now_;
    const double real = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return (real - epoch_) * options_.speed;
}

void MockModel::use_manual_clock(double seconds) {
    manual_clock_ = true;
    manual_now_ = seconds;
}

void MockModel::advance(double seconds) {
    if (manual_clock_) manual_now_ += seconds;
    else epoch_ -= seconds / options_.speed;
}

Phase MockModel::phase() const { return phase_; }
std::string MockModel::version() const { return ST_VERSION_STRING; }
std::string MockModel::game_dir() const { return phase_ == Phase::not_in_game_folder ? kDownloadsDir : kSteamDir; }
std::string MockModel::discovered_game_dir() const { return kSteamDir; }

InstallChecks MockModel::checks() const {
    InstallChecks c;
    c.game_found = phase_ != Phase::not_in_game_folder;
    switch (options_.scenario) {
    case MockScenario::not_installed: c.mod_state = ModState::missing; break;
    case MockScenario::mod_foreign:
        c.mod_state = ModState::foreign;
        c.mod_note = last_error_;
        break;
    case MockScenario::mod_disabled:
        c.mod_state = ModState::disabled;
        c.mod_note = last_error_;
        break;
    case MockScenario::dev_build:
        c.mod_state = ModState::dev_build;
        c.mod_note = "Development build: there is no stranded_together\\package.json, so winhttp.dll is not checked against a release.";
        break;
    case MockScenario::mod_mismatch:
        c.mod_state = ModState::mismatch;
        c.mod_note = "winhttp.dll is not the one from this Stranded Together package (" + std::string(ST_VERSION_STRING) +
                     "). Extract the whole zip into the game folder again; both players need the same version.";
        c.package_version = ST_VERSION_STRING;
        break;
    default:
        c.mod_state = ModState::ok;
        c.package_version = ST_VERSION_STRING;
        break;
    }
    c.mod_present = c.mod_state == ModState::ok || c.mod_state == ModState::dev_build || c.mod_state == ModState::mismatch;
    c.steam_running = options_.scenario != MockScenario::steam_offline;
    c.version_tested = options_.scenario != MockScenario::untested_version;
    if (!c.version_tested)
        c.version_note = "This ds.exe is not the build Stranded Together was made for (Steam may have updated the game). You can "
                         "still play, but co-op may misbehave or crash until the mod is updated.";
    return c;
}

std::string MockModel::last_error() const { return last_error_; }

void MockModel::recheck() {
    if (options_.scenario == MockScenario::mod_disabled) return;   // only Enable mod fixes it
    if (phase_ == Phase::not_in_game_folder || phase_ == Phase::error) {
        options_.scenario = MockScenario::ready;
        last_error_.clear();
        phase_ = Phase::ready;
    }
}

bool MockModel::StartSession(Role role, std::string& error_out) {
    if (phase_ != Phase::ready) {
        error_out = ui::SessionActive(phase_) ? "A game is already running. Close it first." : last_error_;
        return false;
    }
    if (role != Role::echo && role != Role::solo && transport_ == Transport::udp) {
        const std::string problem = ui::LanProblem(lan_);
        if (!problem.empty()) {
            error_out = "LAN mode: " + problem;
            return false;
        }
    }
    session_ = Session{role, role == Role::echo || role == Role::solo ? Transport::udp : transport_, now(), std::string()};
    last_error_.clear();
    phase_ = Phase::launching;
    return true;
}

bool MockModel::host(std::string& code_out, std::string& error_out) {
    code_out.clear();
    if (options_.scenario == MockScenario::steam_offline) {
        error_out = "Start Steam and log in first. Your join code carries your Steam account.";
        return false;
    }
    std::random_device rd;
    std::string secret;
    for (int i = 0; i < 20; ++i) secret.push_back(core::kAlphabet[rd() & 31u]);
    const std::string code = FormatJoinCode(secret, kOwnSteamId);
    if (!StartSession(Role::host, error_out)) return false;
    session_.code = code;
    code_out = code;
    return true;
}

CodeCheck MockModel::check_code(const std::string& code) const { return CheckJoinCode(code, kOwnSteamId); }

bool MockModel::join(const std::string& code, std::string& error_out) {
    const CodeCheck check = check_code(code);
    if (!check.ok) {
        error_out = check.message.empty() ? "Paste the code your partner sent you." : check.message;
        return false;
    }
    return StartSession(Role::join, error_out);
}

bool MockModel::solo_test(std::string& error_out) { return StartSession(Role::echo, error_out); }
bool MockModel::debug_solo(std::string& error_out) { return StartSession(Role::solo, error_out); }

bool MockModel::cancel_launch(std::string& error_out) {
    if (phase_ != Phase::launching) {
        error_out = "The game is already running. Quit it from the game's menu.";
        return false;
    }
    phase_ = Phase::ready;
    return true;
}

bool MockModel::end_session(std::string& error_out) {
    if (phase_ == Phase::in_game) {
        error_out = "DEATH STRANDING is running. Quit it from the game's menu first; co-op tidies up after it.";
        return false;
    }
    if (ui::SessionActive(phase_)) phase_ = Phase::ready;
    return true;
}

Role MockModel::session_role() const { return session_.role; }
Transport MockModel::session_transport() const { return session_.transport; }

std::string MockModel::join_code() const {
    return ui::SessionActive(phase_) && session_.role == Role::host ? session_.code : std::string();
}

bool MockModel::session_pending() const { return ui::SessionActive(phase_); }

double MockModel::GameSeconds() const { return now() - session_.start - kLaunch; }

LinkView MockModel::link() const {
    LinkView v;
    if (!ui::SessionActive(phase_)) return v;
    if (phase_ == Phase::launching) {
        v.headline = "Starting DEATH STRANDING...";
        v.detail = "Steam is starting the game. This can take a minute.";
        return v;
    }
    if (phase_ == Phase::after_game) {
        v.headline = "The game has closed";
        v.detail = "Co-op is switched off again. Starting DEATH STRANDING from Steam now runs it unmodded.";
        return v;
    }
    const double g = GameSeconds();
    const bool host = session_.role == Role::host;
    const bool solo = session_.role == Role::solo;
    const bool echo = session_.role == Role::echo || solo;
    const bool udp = session_.transport == Transport::udp;
    core::json::Value s = core::json::Value::Object();
    s["v"] = 1;
    s["pid"] = 4242;
    s["transport"] = udp ? "udp" : "steam";
    s["route"] = udp ? "direct" : "relay";
    s["peer"] = echo ? "" : (host ? "Fragile" : "Sam");
    s["rtt_ms"] = 0;
    s["remote"] = "none";
    s["reason"] = "";
    s["code"] = host ? session_.code : std::string();
    std::string state = host ? "listening" : "connecting";
    if (echo) {
        state = "idle";
        if (g >= kConnectAt) s["remote"] = g >= kVisibleAt ? "visible" : "spawning";
    } else if (g >= kConnectAt) {
        state = "connected";
        s["remote"] = g >= kVisibleAt ? "visible" : "spawning";
        const int spike = (static_cast<int>(g) % 11 == 0) ? 31 : 0;
        s["rtt_ms"] = 44 + static_cast<int>(std::lround(9.0 * std::sin(g * 0.7))) + spike;
        if ((options_.scenario == MockScenario::partner_drops && g >= kDropAt) || link_override_ == LinkOverride::lost) {
            state = "lost";
            s["remote"] = "none";
            s["rtt_ms"] = 0;
            s["reason"] = "Fragile closed the game";
        }
    }
    s["state"] = state;
    const bool fresh = link_override_ != LinkOverride::not_responding;
    return FromStatusView(core::StatusViewOf(s, fresh, solo ? "solo" : echo ? "echo" : host ? "host" : "guest"));
}

std::string MockModel::mod_dir() const { return std::string(kSteamDir) + "\\stranded_together"; }
std::string MockModel::mod_log_path() const { return mod_dir() + "\\stranded_together.log"; }

bool MockModel::collect_logs(std::string& path_out, std::string& error_out) {
    path_out.clear();
    if (phase_ == Phase::not_in_game_folder) {
        error_out = "Start Stranded Together from your game folder first.";
        return false;
    }
    path_out = DesktopFile("stranded-together-logs-" + Stamp(manual_clock_) + ".zip");
    return true;
}

std::string MockModel::mod_log_tail(std::size_t max_bytes) const {
    if (phase_ == Phase::not_in_game_folder) return {};
    static const char* const kLines[] = {
        "[12:00:01.204] Stranded Together 0.1.0 loaded into ds.exe (pid 4242)",
        "[12:00:01.206] [config] role=echo transport=udp peer=0 udp=127.0.0.1:27555->27556 send_hz=30 interp=100ms debug_menu=1 key=0x78",
        "[12:00:01.207] [config] one-shot session: consumed (renamed to session.last.ini)",
        "[12:00:01.512] [game] signatures: 9/9 found",
        "[12:00:02.031] [hook] frame hook installed",
        "[12:00:41.880] [game] local player found",
        "[12:00:42.115] [echo] clone spawned 1.5 m to the side, replaying 2.0 s behind",
        "[12:00:42.116] [menu] debug menu ready: F9",
        "[12:01:05.402] [status] link-status.json state=idle remote=visible",
    };
    std::string text;
    for (const char* line : kLines) {
        text += line;
        text += "\n";
    }
    if (text.size() > max_bytes) text = text.substr(text.size() - max_bytes);
    return text;
}

bool MockModel::crash_report(std::string& text_out) const {
    text_out.clear();
    if (!crash_report_) return false;
    text_out = "Unhandled exception at 0x0000000140A1B2C3 (ds.exe): access violation reading 0x0000000000000010\n";
    return true;
}

bool MockModel::set_mod_enabled(bool enabled, std::string& error_out) {
    if (ui::SessionActive(phase_)) {
        error_out = "A game session is running. Switch the mod on or off after the game has closed.";
        return false;
    }
    if (enabled && options_.scenario == MockScenario::mod_disabled) {
        set_scenario(MockScenario::ready);
        return true;
    }
    if (!enabled && phase_ == Phase::ready) {
        set_scenario(MockScenario::mod_disabled);
        return true;
    }
    error_out = enabled ? "There is no winhttp.dll.disabled to switch back on." : "There is no winhttp.dll to switch off.";
    return false;
}

bool MockModel::clear_session(std::string& error_out) {
    if (phase_ == Phase::in_game) {
        error_out = "DEATH STRANDING is running. Quit it from the game's menu first.";
        return false;
    }
    if (ui::SessionActive(phase_)) phase_ = Phase::ready;
    return true;
}

// Like the real model: the stored pick is an offered character or Fragile (porter suit), set only
// with the Debug option "Allow other characters"; without it the guest is always Fragile (porter suit).
void MockModel::set_my_character(const std::string& key) {
    if (debug_.allow_other_characters) my_character_ = core::GuestPickOrDefault(key);
}
void MockModel::set_local_test_guest_character(const std::string& key) {
    if (debug_.allow_other_characters) guest_character_ = core::GuestPickOrDefault(key);
}
std::string MockModel::my_character() const { return core::GuestCharacter(my_character_, debug_.allow_other_characters); }

LocalTestView MockModel::local_test() const {
    LocalTestView v;
    v.host_pad = host_pad_;
    v.fps = fps_;
    v.guest_character = core::GuestCharacter(guest_character_, debug_.allow_other_characters);
    if (local_start_ < 0.0) return v;
    const double t = now() - local_start_;
    v.running = true;
    v.host_pid = 4242;
    v.guest_pid = t >= 8.0 ? 4343u : 0u;
    v.host_state = t >= 20.0 ? "connected" : "listening";
    v.guest_state = t >= 20.0 ? "connected" : t >= 8.0 ? "connecting" : "";
    const bool linked = t >= 20.0;
    v.headline = linked ? "Both games are linked" : t >= 8.0 ? "Both games are running" : "Starting the first game (host)...";
    v.detail = linked ? "Each window shows the other player. Keyboard + mouse play the host window, a pad plays the guest."
                      : "Load a save in each window (Continue). The second game may first say it cannot reach the game server: press OK.";
    v.tone = linked ? LinkView::Tone::good : LinkView::Tone::neutral;
    return v;
}

bool MockModel::start_local_test(std::string& error_out) {
    if (phase_ != Phase::ready || local_start_ >= 0.0) {
        error_out = "A game is already running. Close it first.";
        return false;
    }
    local_start_ = now();
    return true;
}

void MockModel::jump_to_local_test(double seconds) { local_start_ = seconds < 0.0 ? -1.0 : now() - seconds; }

void MockModel::tick() {
    if (local_start_ >= 0.0 && now() - local_start_ >= 90.0) {
        local_start_ = -1.0;
        test_.result = "Your own saves were not changed (every file checked by SHA-256 before and after).";
        test_.tone = TestSaveView::Tone::good;
    }
    if (update_started_ >= 0.0) {
        const double u = now() - update_started_;
        if (update_.status == UpdateStatus::checking && u >= 1.5) {
            const bool auto_check = update_.auto_check;
            update_ = MockRelease();
            update_.auto_check = auto_check;
            update_started_ = -1.0;
        } else if (update_.status == UpdateStatus::downloading) {
            update_.progress = static_cast<float>(std::fmin(1.0, u / 4.0));
            update_.bytes_downloaded = static_cast<std::uint64_t>(update_.progress * static_cast<double>(update_.download_size));
            if (u >= 4.0) update_.status = UpdateStatus::applying;
        } else if (update_.status == UpdateStatus::applying && u >= 5.5) {
            // The real launcher restarts here; the mock stays open as the "new" one.
            update_.updated_to = update_.latest_version;
            update_.current_version = update_.latest_version;
            update_.status = UpdateStatus::up_to_date;
            update_started_ = -1.0;
        }
    }
    if (!ui::SessionActive(phase_)) return;
    const double s = now() - session_.start;
    if (s < kLaunch) phase_ = Phase::launching;
    else if (s < kLaunch + kGameLength) phase_ = Phase::in_game;
    else if (s < kLaunch + kGameLength + kAfterGame) phase_ = Phase::after_game;
    else phase_ = Phase::ready;
}

void MockModel::jump_to_phase(Phase phase) {
    last_error_.clear();
    link_override_ = LinkOverride::none;
    if (phase == Phase::error) {
        set_scenario(MockScenario::not_installed);
        return;
    }
    if (phase == Phase::not_in_game_folder) {
        set_scenario(MockScenario::wrong_folder);
        return;
    }
    phase_ = phase;
}

void MockModel::jump_to_session(Role role, Transport transport, Phase phase, double seconds) {
    double offset = seconds;
    if (phase == Phase::in_game) offset += kLaunch;
    if (phase == Phase::after_game) offset += kLaunch + kGameLength;
    session_ = Session{role, transport, now() - offset, std::string()};
    if (role == Role::host) session_.code = FormatJoinCode("K7RD3WQ9MP2XVB6TZHN4", kOwnSteamId);
    last_error_.clear();
    phase_ = Phase::launching;
    tick();
}

std::unique_ptr<LauncherModel> MakeMockModel(const MockOptions& options) { return std::make_unique<MockModel>(options); }

}  // namespace st
