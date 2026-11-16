#pragma once
// Stranded Together: the MOCK LauncherModel. It simulates every phase on a
// clock so the GUI runs without the game: host codes, join-code checks (the
// real DST1 parser), the game starting, the link status (built with the same
// StatusViewOf the real model uses), the game closing and the Tools results.
// It never touches the disk, Steam, the registry or the game.
// Adapted from Diamond Dogs Together's launcher_model_mock.
//
// Timeline at speed 1 (seconds): the game starts 4 s after Host/Join/Solo; the
// partner connects 5 s into the game and appears at 8 s; the game closes by
// itself after 90 s; tidying up takes 4 s, then the launcher is ready again.
#include <cstdint>
#include <string>

#include "launcher_model.hpp"

namespace st {

enum class MockScenario {
    ready,             // the default
    wrong_folder,      // not_in_game_folder; "Check again" moves on to ready
    not_installed,     // error: winhttp.dll missing; "Check again" fixes it
    untested_version,  // ready, with the untested-version warning
    steam_offline,     // Steam not running: Host is disabled
    partner_drops,     // the partner leaves 34 s into the game
    dev_build,         // ready: winhttp.dll is ours but there is no package.json
    mod_mismatch,      // ready, with the "not the package's winhttp.dll" warning
    mod_foreign,       // error: another program's winhttp.dll
    mod_disabled,      // error: only winhttp.dll.disabled; Enable mod fixes it
    update_available,  // ready, and a newer release is out (Update now simulates download, install, restart)
};
bool ParseMockScenario(const std::string& name, MockScenario& out);

struct MockOptions {
    MockScenario scenario = MockScenario::ready;
    double speed = 1.0;
};

class MockModel final : public LauncherModel {
public:
    explicit MockModel(const MockOptions& options);

    Phase phase() const override;
    std::string version() const override;
    std::string game_dir() const override;
    std::string discovered_game_dir() const override;
    InstallChecks checks() const override;
    std::string last_error() const override;
    void recheck() override;

    Transport transport() const override { return transport_; }
    void set_transport(Transport t) override { transport_ = t; }
    LanSettings lan() const override { return lan_; }
    void set_lan(const LanSettings& lan) override { lan_ = lan; }
    std::string player_name() const override { return name_; }
    void set_player_name(const std::string& name) override { name_ = name; }

    bool host(std::string& code_out, std::string& error_out) override;
    CodeCheck check_code(const std::string& code) const override;
    bool join(const std::string& code, std::string& error_out) override;
    bool solo_test(std::string& error_out) override;
    bool debug_solo(std::string& error_out) override;
    bool cancel_launch(std::string& error_out) override;
    bool end_session(std::string& error_out) override;

    Role session_role() const override;
    Transport session_transport() const override;
    std::string join_code() const override;
    LinkView link() const override;
    bool session_pending() const override;

    std::string mod_dir() const override;
    std::string mod_log_path() const override;
    bool collect_logs(std::string& path_out, std::string& error_out) override;

    DebugOptions debug_options() const override { return debug_; }
    void set_debug_options(const DebugOptions& options) override { debug_ = options; }
    std::string mod_log_tail(std::size_t max_bytes) const override;
    bool has_crash_report() const override { return crash_report_; }
    bool crash_report(std::string& text_out) const override;
    bool set_mod_enabled(bool enabled, std::string& error_out) override;
    bool clear_session(std::string& error_out) override;
    // Updates: Check for updates finds MockRelease() after 1.5 s; Update now downloads for
    // 4 s, installs for 1.5 s and then reports "Updated to" (the window stays open).
    UpdateInfo update_info() const override;
    void check_for_updates() override;
    void start_update() override;
    void set_auto_update_check(bool on) override { update_.auto_check = on; }
    void tick() override;

    // ---- Hooks for tests and the screenshot driver -------------------------
    static constexpr std::uint64_t kOwnSteamId = 76561197960265728ull + 104729ull;
    static constexpr std::uint64_t kPartnerSteamId = 76561197960265728ull + 7919ull;
    static std::string PartnerCode();
    void use_manual_clock(double seconds);
    void advance(double seconds);
    double now() const;
    void set_scenario(MockScenario scenario);   // resets the phase for that scenario
    void jump_to_phase(Phase phase);            // ready, error, not_in_game_folder
    // A session as `role` over `transport`, `seconds` into `phase` (launching, in_game or after_game).
    void jump_to_session(Role role, Transport transport, Phase phase, double seconds);
    enum class LinkOverride { none, not_responding, lost };
    void set_link_override(LinkOverride o) { link_override_ = o; }
    void set_crash_report(bool present) { crash_report_ = present; }
    static UpdateInfo MockRelease();                          // "v0.2.0" with notes, status available
    void set_update_info(const UpdateInfo& info) { update_ = info; update_started_ = -1.0; }

private:
    struct Session {
        Role role = Role::host;
        Transport transport = Transport::steam;
        double start = 0.0;   // when Host/Join/Solo was pressed
        std::string code;     // the host's code
    };
    double GameSeconds() const;   // since the game started (negative while launching)
    bool StartSession(Role role, std::string& error_out);

    MockOptions options_;
    bool manual_clock_ = false;
    double manual_now_ = 0.0;
    double epoch_ = 0.0;

    Phase phase_ = Phase::ready;
    Session session_;
    Transport transport_ = Transport::steam;
    LanSettings lan_;
    std::string name_;
    std::string last_error_;
    LinkOverride link_override_ = LinkOverride::none;
    DebugOptions debug_;
    bool crash_report_ = false;
    UpdateInfo update_;
    double update_started_ = -1.0;   // when Check / Update now was pressed (mock clock)
};

std::unique_ptr<LauncherModel> MakeMockModel(const MockOptions& options);

}  // namespace st
