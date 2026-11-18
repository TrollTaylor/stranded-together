#pragma once
// Stranded Together: the GUI pages (Play, In game, Tools, Debug, About) drawn with
// Dear ImGui over a LauncherModel. The Win32/DX11 shell (launcher_main.cpp)
// owns the window and calls Frame() once per redraw. Adapted from Diamond Dogs
// Together's launcher_app (same layout and widgets; DS pages and texts).
#include <functional>
#include <string>

#include "imgui.h"

struct ImGuiWindow;   // imgui_internal.h (the page's window, launcher_app.cpp)
#include "launcher_model.hpp"
#include "launcher_ui_logic.hpp"

namespace st {

class MockModel;

// What the window shell does for the pages (Explorer, the shell).
struct ShellServices {
    std::function<void(const std::string& utf8_folder)> open_folder;
    std::function<void(const std::string& utf8_file)> reveal_file;   // Explorer with the file selected
    std::function<void(const std::string& utf8_file)> open_file;     // the file's default program (a .log in Notepad)
    std::function<void(const std::string& utf8_url)> open_url;
    // A folder picker (Debug > Test saves); false when cancelled. Null in screenshots.
    std::function<bool(std::string& utf8_folder_out)> pick_folder;
};

struct LogoTexture {
    ImTextureID id = 0;
    float width = 0.0f;   // pixels of the source image
    float height = 0.0f;
};

class LauncherUi {
public:
    LauncherUi(LauncherModel& model, ShellServices shell, LogoTexture logo);

    // Builds one frame (call between ImGui::NewFrame and ImGui::Render).
    // `time` is a monotonic clock in seconds (spinners, "Copied" feedback).
    void Frame(double time);
    bool Animating() const { return animating_; }

    // ---- Screenshot driver (--screenshot-all) ------------------------------
    static int SceneCount();
    static const char* SceneName(int index);
    // Resets the pages and puts `mock` and the pages into scene `index`.
    void PrepareScene(int index, MockModel& mock);

private:
    struct Result {
        bool ok = false;
        std::string text;
        std::string path;   // a file to reveal ("Show in folder")
    };

    void Sidebar(Phase phase);
    void PageHeader(const char* title, const char* subtitle);
    void ChecksStrip(const InstallChecks& checks, Phase phase);
    void WrongFolderPage();
    void NotReadyCard();
    void PlayPage(Phase phase);
    void HostTab(Phase phase, const InstallChecks& checks);
    void JoinTab(Phase phase);
    void SoloTab(Phase phase);
    void AdvancedSection(bool session);
    void SessionNote(Phase phase);
    void CodeCard(bool compact);
    void InGamePage(Phase phase);
    void ToolsPage(Phase phase);
    void DebugPage(Phase phase);
    void DebugOptionsTab(Phase phase);
    void DebugFilesTab(Phase phase);
    void DebugSavesTab(Phase phase);
    void ModSwitchButton(const InstallChecks& checks, bool enabled, float logical_width);
    void RefreshLog(bool force);
    void AboutPage(Phase phase);
    void UpdateCard(Phase phase);       // on top of Play: update available / downloading / failed / updated
    void UpdatesSection(Phase phase);   // About: status, Check for updates / Update now, the auto-check setting
    void ResultLine(Result& result);
    void CopyCode();
    void Navigate(ui::Page page);
    void LoadFields();
    void CommitFields();

    LauncherModel& model_;
    ShellServices shell_;
    LogoTexture logo_;
    double time_ = 0.0;
    bool animating_ = false;
    bool first_frame_ = true;

    Phase last_phase_ = Phase::ready;
    ui::Page page_ = ui::Page::play;
    ui::PlayTab play_tab_ = ui::PlayTab::host;

    // Play
    char code_buf_[160] = {};
    std::string checked_text_ = "\x01";
    CodeCheck checked_;
    std::string hosted_code_;
    std::string play_error_;
    double copied_until_ = -1.0;
    bool advanced_open_ = false;
    char ip_buf_[40] = {};
    char port_local_buf_[8] = {};
    char port_peer_buf_[8] = {};
    char name_buf_[40] = {};
    bool open_character_list_ = false;   // screenshot scene: open the Join tab's character list once
    bool scroll_to_partner_body_ = false;   // screenshot scene: Debug > Options scrolled to PARTNER BODY
    ::ImGuiWindow* page_window_ = nullptr;   // the scrolling page (this frame)

    LinkView link_;   // this frame's link status (sessions only)

    // Tools
    Result tool_result_;
    Result logs_result_;   // Collect logs from the not-ready card

    // Debug
    ui::DebugTab debug_tab_ = ui::DebugTab::options;
    Result debug_result_;
    std::string log_text_;           // the last lines of the mod's log, refreshed about once a second
    double log_refreshed_at_ = -1e9;
    bool log_follow_ = true;         // keep the live log scrolled to its end
    bool log_scroll_end_ = false;
    DebugOptions debug_edit_;        // a slider being dragged (saved when it is released)
    bool debug_editing_ = false;
};

}  // namespace st
