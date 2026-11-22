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
    std::string my_character() const override;
    void set_my_character(const std::string& key) override;

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

    // Test saves: a picked folder is "ok" when its name contains "save"; Play with test save
    // runs a solo session; the local test runs for 90 s (linked after 45 s) and then ends.
    TestSaveView test_save() const override { return test_; }
    bool set_test_save_folder(const std::string& folder, std::string& error_out) override {
        if (!folder.empty() && folder.find("save") == std::string::npos && folder.find("Save") == std::string::npos) {
            error_out = "No save slots there. Pick the folder that holds DeathStrandingDC (with autosave/manualsave folders inside).";
            return false;
        }
        test_.folder = folder;
        test_.folder_ok = !folder.empty();
        test_.folder_note = folder.empty() ? "No test save picked yet." : "23 save slots found";
        test_.seeded = false;
        return true;
    }
    void set_use_test_save(bool on) override { test_.use_for_all = on; }
    bool play_test_save(std::string& error_out) override {
        if (!test_.folder_ok) {
            error_out = "Pick a test save folder first (Debug > Test saves).";
            return false;
        }
        test_.seeded = true;
        return debug_solo(error_out);
    }
    bool reset_test_saves(std::string& error_out) override {
        if (local_start_ >= 0.0) {
            error_out = "Quit DEATH STRANDING first: the test save copies are in use.";
            return false;
        }
        test_.seeded = false;
        test_.result.clear();
        return true;
    }
    LocalTestView local_test() const override;
    void set_local_test_host_pad(bool on) override { host_pad_ = on; }
    void set_local_test_fps(int fps) override { fps_ = fps; }
    void set_local_test_guest_character(const std::string& key) override;
    bool start_local_test(std::string& error_out) override;
    // Tools > Fragile's textures: a simulated extraction (6 s on the mock clock) into a placeholder folder.
    TexturesView textures() const override;
    bool extract_textures(std::string& error_out) override;
    void cancel_textures() override;
    bool check_textures(std::string& error_out) override;
    bool textures_folder_for_open(std::string& path_out, std::string& error_out) override;
    bool set_textures_folder(const std::string& folder, std::string& error_out) override;
    void set_boots_look(bool own) override;
    // screenshot driver: the card in a given state (not_extracted, extracting at `progress`, extracted with edits)
    void set_textures_state(TexturesView::State state, float progress = 0.0f);
    // screenshot driver: the local test `seconds` after it started (< 0: not running)
    void jump_to_local_test(double seconds);
    void set_test_save_view(const TestSaveView& v) { test_ = v; }

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
    std::string my_character_ = "fragile_suit";   // core::kGuestCharacter
    std::string last_error_;
    LinkOverride link_override_ = LinkOverride::none;
    DebugOptions debug_;
    bool crash_report_ = false;
    UpdateInfo update_;
    double update_started_ = -1.0;   // when Check / Update now was pressed (mock clock)
    TestSaveView test_ = [] {
        TestSaveView v;
        v.folder_note = "No test save picked yet.";
        v.sandbox_dir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\DEATH STRANDING DIRECTORS CUT\\stranded_together\\sandbox";
        return v;
    }();
    bool host_pad_ = false;
    int fps_ = 60;
    std::string guest_character_ = "fragile_suit";   // core::kGuestCharacter
    double local_start_ = -1.0;   // mock clock; < 0 when no local test runs
    std::string tex_folder_;          // "" = the default placeholder
    bool tex_boots_own_ = true;       // her boots: her own, painted / Sam's boots
    bool tex_extracted_ = false;
    double tex_start_ = -1.0;         // mock clock; < 0 when no extraction runs
    float tex_frozen_ = -1.0f;        // screenshot driver: a fixed progress
    std::string tex_result_, tex_report_;
    TexturesView::Tone tex_tone_ = TexturesView::Tone::neutral;
};

std::unique_ptr<LauncherModel> MakeMockModel(const MockOptions& options);

}  // namespace st
