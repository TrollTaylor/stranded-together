// Stranded Together: the GUI pages (see launcher_app.hpp).
#include "launcher_app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/core_text.hpp"   // the character list (native/mod/body_table.cpp's offered bodies)
#include "imgui_internal.h"   // ClosePopupToLevel (the screenshot driver resets popups between scenes)
#include "launcher_join_code.hpp"
#include "launcher_model_mock.hpp"
#include "launcher_theme.hpp"

namespace st {
namespace {
using namespace theme;
using ui::Page;

constexpr float kSidebarWidth = 204.0f;
// Debug > Partner body = Hologram: shown next to every "you appear as" choice, because a hologram
// game sends no joint frames and so never shows its character to the partner.
constexpr const char* kHologramNote = "Partner body is Hologram (Debug page): your partner sees you as a hologram.";
// Debug > "Host holds the save" off (session.ini host_save=0): the features that need the shared save are off in the game.
constexpr const char* kHostSaveOffNote =
    "Host holds the save is off (Debug page): no waiting screen, no shared vehicles, no riding together, no vehicle hits. "
    "Each player plays their own save and the partner's vehicles are the old see-through copies.";

ImU32 ToneColor(ui::Tone tone) {
    switch (tone) {
    case ui::Tone::good: return col::kGood;
    case ui::Tone::warn: return col::kWarn;
    case ui::Tone::bad: return col::kBad;
    case ui::Tone::neutral: break;
    }
    return col::kTextDim;
}

ImU32 LinkToneColor(LinkView::Tone tone) {
    switch (tone) {
    case LinkView::Tone::good: return col::kGood;
    case LinkView::Tone::warn: return col::kWarn;
    case LinkView::Tone::bad: return col::kBad;
    case LinkView::Tone::neutral: break;
    }
    return col::kCool;
}

Icon PageIcon(Page page) {
    switch (page) {
    case Page::play: return Icon::play;
    case Page::in_game: return Icon::signal;
    case Page::tools: return Icon::tools;
    case Page::debug: return Icon::sliders;
    case Page::about: return Icon::info;
    }
    return Icon::dot;
}

std::string FileName(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void Gap(float logical) { ImGui::Dummy(ImVec2(0.0f, Px(logical))); }

// An icon on a soft round badge; the cursor then moves right of it.
void IconBadge(Icon icon, ImU32 color, ImU32 background, float logical_size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float s = Px(logical_size);
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
    d->AddCircleFilled(c, s * 0.5f, background);
    DrawIcon(d, icon, c, s * 0.5f, color);
    ImGui::Dummy(ImVec2(s, s));
}

// A message with a leading icon in a tone colour, wrapped to `wrap_width`
// (from the current cursor; 0 = the rest of the region).
void Message(Icon icon, ImU32 color, const std::string& text, float size = kSmall, float wrap_width = 0.0f, ImU32 text_color = 0) {
    if (text.empty()) return;
    const Font f(GetFonts().regular, size);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float start_x = ImGui::GetCursorPosX();
    const float line = ImGui::GetTextLineHeight();
    DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x + Px(7.0f), p.y + line * 0.5f), Px(14.0f), color);
    ImGui::SetCursorScreenPos(ImVec2(p.x + Px(20.0f), p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, text_color ? text_color : color);
    ImGui::PushTextWrapPos(wrap_width > 0.0f ? start_x + wrap_width : 0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

const char* PhaseStatus(Phase phase, const LinkView& link, Role role, ImU32& color) {
    switch (phase) {
    case Phase::not_in_game_folder: color = col::kBad; return "Wrong folder";
    case Phase::error: color = col::kBad; return "Not ready";
    case Phase::ready: color = col::kGood; return "Ready to play";
    case Phase::launching: color = col::kAccent; return "Starting the game";
    case Phase::in_game:
        if (role == Role::echo) {
            color = link.connected ? col::kGood : col::kCool;
            return "Solo test";
        }
        if (role == Role::solo) {
            color = link.connected ? col::kGood : col::kCool;
            return "Debug tools";
        }
        color = link.connected ? col::kGood : col::kCool;
        return link.connected ? "Connected" : "In game";
    case Phase::after_game: color = col::kTextDim; return "Game closed";
    }
    color = col::kTextDim;
    return "";
}

// The sidebar title: the logo's spaced, thin lettering with its drip lines.
void SpacedTitle(ImDrawList* d, float center_x, float y, const char* line, float tracking, unsigned seed, float size = 15.0f) {
    ImFont* font = GetFonts().light;
    const float w = SpacedTextWidth(font, size, line, tracking);
    const float x0 = std::floor(center_x - w * 0.5f);
    SpacedText(d, font, size, ImVec2(x0, y), col::kText, line, tracking);
    // Drip lines under a few letters (deterministic, like the logo's).
    const Font f(font, size);
    float x = x0;
    unsigned h = seed;
    for (const char* p = line; *p; ++p) {
        const float cw = ImGui::CalcTextSize(p, p + 1).x;
        h = h * 1103515245u + 12345u;
        if (*p != ' ' && ((h >> 16) % 3u) != 0u) {
            const float len = Px(4.0f + static_cast<float>((h >> 8) % 13u));
            const float lx = std::floor(x + cw * (0.30f + 0.4f * static_cast<float>((h >> 4) % 5u) / 4.0f)) + 0.5f;
            const float top = y + ImGui::GetTextLineHeight() - Px(1.0f);
            d->AddRectFilledMultiColor(ImVec2(lx, top), ImVec2(lx + std::fmax(1.0f, Px(0.8f)), top + len), col::Rgb(0x9A, 0xA6, 0xB1, 0x90),
                                       col::Rgb(0x9A, 0xA6, 0xB1, 0x90), col::Rgb(0x9A, 0xA6, 0xB1, 0x00), col::Rgb(0x9A, 0xA6, 0xB1, 0x00));
        }
        x += cw + Px(tracking);
    }
}

// A small rounded chip: icon + label, in a tone.
void Chip(const char* label, Icon icon, ImU32 color, ImU32 background) {
    const Font f(GetFonts().semibold, kLabel + 1.0f);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ts.x + Px(36.0f), Px(28.0f));
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), background, size.y * 0.5f);
    DrawIcon(d, icon, ImVec2(p.x + Px(15.0f), p.y + size.y * 0.5f), Px(14.0f), color);
    d->AddText(ImVec2(p.x + Px(27.0f), std::floor(p.y + (size.y - ts.y) * 0.5f)), col::kText, label);
    ImGui::Dummy(size);
}

// A labelled single-line input in the theme; returns true when edited.
bool Field(const char* id, const char* label, char* buf, std::size_t size, float logical_width, bool enabled, const char* hint = nullptr,
           ImGuiInputTextFlags flags = 0) {
    ImGui::BeginGroup();
    SectionLabel(label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    const Font f(GetFonts().mono, kSmall + 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(10.0f, 8.0f));
    ImGui::SetNextItemWidth(Px(logical_width));
    if (!enabled) ImGui::BeginDisabled();
    const bool changed = hint ? ImGui::InputTextWithHint(id, hint, buf, size, flags) : ImGui::InputText(id, buf, size, flags);
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    return changed;
}
// A labelled slider in the theme; returns true while it changes the value.
bool SliderFloatField(const char* id, const char* label, float* value, float lo, float hi, const char* format, float logical_width) {
    ImGui::BeginGroup();
    SectionLabel(label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    const Font f(GetFonts().mono, kSmall + 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, Px(12.0f));
    ImGui::SetNextItemWidth(Px(logical_width));
    const bool changed = ImGui::SliderFloat(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar(2);
    ImGui::EndGroup();
    return changed;
}

bool SliderIntField(const char* id, const char* label, int* value, int lo, int hi, const char* format, float logical_width) {
    ImGui::BeginGroup();
    SectionLabel(label);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    const Font f(GetFonts().mono, kSmall + 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, Px(12.0f));
    ImGui::SetNextItemWidth(Px(logical_width));
    const bool changed = ImGui::SliderInt(id, value, lo, hi, format, ImGuiSliderFlags_AlwaysClamp);
    ImGui::PopStyleVar(2);
    ImGui::EndGroup();
    return changed;
}

// The guest's fixed character (the Debug option "Allow other characters" off): in the combo's place
// and of its height, a frame with a person icon and the label; nothing to open.
void FixedCharacter(const std::string& label, float logical_width, bool enabled) {
    const Font f(GetFonts().regular, kSmall + 1.0f);
    const float h = ImGui::GetTextLineHeight() + Px(16.0f);   // the combo's frame: padding 8 above and below
    const float w = Px(logical_width);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col::kField, Px(7.0f));
    d->AddRect(p, ImVec2(p.x + w, p.y + h), col::kBorder, Px(7.0f), 0, Px(1.0f));
    // dimmed like the combo while it cannot change (connected, a local test running)
    DrawIcon(d, Icon::person, ImVec2(p.x + Px(17.0f), p.y + h * 0.5f), Px(14.0f), enabled ? col::kAccent : col::kTextFaint);
    d->AddText(ImVec2(p.x + Px(32.0f), p.y + Px(8.0f)), enabled ? col::kText : col::kTextDim, label.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

// The character list (Join, the local test): core::GuestCharacterChoices, by their labels (every
// offered body of native/mod/body_table.cpp with the Debug option "Allow other characters", else
// Fragile (porter suit) alone, drawn as FixedCharacter). Returns true when another character was
// picked (*key is set to its key). open: open the list now (the screenshot scene that shows it).
bool CharacterCombo(const char* id, std::string* key, float logical_width, bool enabled, bool allow_others, bool open = false) {
    if (!allow_others) {
        FixedCharacter(core::CharacterLabel(core::kGuestCharacter), logical_width, enabled);
        return false;
    }
    const std::vector<core::CharacterChoice> choices = core::GuestCharacterChoices(true);
    const std::string current = core::CharacterLabel(*key);
    bool picked = false;
    const Font f(GetFonts().regular, kSmall + 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(10.0f, 8.0f));
    // The open list: roomier rows, and room above and below them (the page's WindowPadding is 0,
    // so the first and last rows would touch the rounded border; BeginCombo sets only x).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Px(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(10.0f, 8.0f));
    ImGui::SetNextItemWidth(Px(logical_width));
    // The list is the offered bodies: no height cap, so it never scrolls. All 16 fit the window at
    // scale 1 (screenshot scene 38-play-join-characters); a longer table would need a cap here.
    if (open) ImGui::OpenPopupEx(ImHashStr("##ComboPopup", 0, ImGui::GetID(id)));   // BeginCombo's own popup id
    if (!enabled) ImGui::BeginDisabled();
    if (ImGui::BeginCombo(id, current.c_str(), ImGuiComboFlags_HeightLargest)) {
        for (const core::CharacterChoice& c : choices) {
            const bool selected = c.key == *key;
            if (ImGui::Selectable(c.label.c_str(), selected) && !selected) {
                *key = c.key;
                picked = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopStyleVar(3);
    return picked;
}

ImU32 SoftOf(ui::Tone tone) {
    switch (tone) {
    case ui::Tone::good: return col::kGoodSoft;
    case ui::Tone::warn: return col::kWarnSoft;
    case ui::Tone::bad: return col::kBadSoft;
    case ui::Tone::neutral: break;
    }
    return col::kCoolSoft;
}

std::size_t CountLines(const std::string& text) {
    std::size_t n = text.empty() ? 0 : 1;
    for (char c : text)
        if (c == '\n') ++n;
    return n;
}
}  // namespace

LauncherUi::LauncherUi(LauncherModel& model, ShellServices shell, LogoTexture logo)
    : model_(model), shell_(std::move(shell)), logo_(logo) {
    LoadFields();
}

void LauncherUi::Navigate(Page page) {
    if (ui::PageEnabled(model_.phase(), page)) page_ = page;
}

void LauncherUi::CopyCode() {
    if (hosted_code_.empty()) return;
    ImGui::SetClipboardText(hosted_code_.c_str());
    copied_until_ = time_ + 2.5;
}

void LauncherUi::LoadFields() {
    const LanSettings lan = model_.lan();
    strncpy_s(ip_buf_, sizeof(ip_buf_), lan.peer_ip.c_str(), _TRUNCATE);
    std::snprintf(port_local_buf_, sizeof(port_local_buf_), "%d", lan.port_local);
    std::snprintf(port_peer_buf_, sizeof(port_peer_buf_), "%d", lan.port_peer);
    strncpy_s(name_buf_, sizeof(name_buf_), model_.player_name().c_str(), _TRUNCATE);
}

void LauncherUi::CommitFields() {
    LanSettings lan = model_.lan();
    lan.peer_ip = ip_buf_;
    int port = 0;
    lan.port_local = ui::ParsePort(port_local_buf_, port) ? port : 0;
    lan.port_peer = ui::ParsePort(port_peer_buf_, port) ? port : 0;
    const LanSettings old = model_.lan();
    if (lan.peer_ip != old.peer_ip || lan.port_local != old.port_local || lan.port_peer != old.port_peer) model_.set_lan(lan);
    if (model_.player_name() != name_buf_) model_.set_player_name(name_buf_);
}

void LauncherUi::Frame(double time) {
    time_ = time;
    animating_ = copied_until_ > time_;
    const Phase phase = model_.phase();
    if (first_frame_) {
        first_frame_ = false;
        page_ = ui::DefaultPage(phase);
        last_phase_ = phase;
    } else if (phase != last_phase_) {
        page_ = ui::PageAfterPhaseChange(last_phase_, phase, page_);
        if (!ui::SessionActive(phase)) {
            hosted_code_.clear();
            copied_until_ = -1.0;
        }
        last_phase_ = phase;
    }
    if (ui::SessionActive(phase)) {
        // A window opened while the game already runs learns the session here.
        if (model_.session_role() == Role::host && hosted_code_.empty()) hosted_code_ = model_.join_code();
    }
    if (!ui::PageEnabled(phase, page_)) page_ = ui::DefaultPage(phase);
    link_ = ui::SessionActive(phase) ? model_.link() : LinkView{};
    if (phase == Phase::launching || phase == Phase::after_game) animating_ = true;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    Sidebar(phase);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(28.0f, 22.0f));
    ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    page_window_ = ImGui::GetCurrentWindow();
    ImGui::PopStyleVar(2);
    if (phase == Phase::not_in_game_folder) {
        WrongFolderPage();
    } else {
        switch (page_) {
        case Page::play: PlayPage(phase); break;
        case Page::in_game: InGamePage(phase); break;
        case Page::tools: ToolsPage(phase); break;
        case Page::debug: DebugPage(phase); break;
        case Page::about: AboutPage(phase); break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---- Sidebar ---------------------------------------------------------------
void LauncherUi::Sidebar(Phase phase) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::kSidebar);
    ImGui::BeginChild("##nav", ImVec2(Px(kSidebarWidth), 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float w = Px(kSidebarWidth);
    const float h = ImGui::GetWindowHeight();
    d->AddLine(ImVec2(origin.x + w - 0.5f, origin.y), ImVec2(origin.x + w - 0.5f, origin.y + h), col::kBorder);
    // A soft amber glow behind the emblem (stacked faint discs: a radial falloff).
    const float cx = origin.x + w * 0.5f;
    for (int i = 0; i < 14; ++i) {
        const float r = Px(86.0f - 5.5f * static_cast<float>(i));
        d->AddCircleFilled(ImVec2(cx, origin.y + Px(64.0f)), r, col::Rgb(0xE8, 0xA3, 0x3A, 0x03), 64);
    }

    // The emblem (pods and cable) and the spaced title.
    const float logo_w = Px(156.0f);
    const float logo_h = logo_.width > 0 ? logo_w * logo_.height / logo_.width : Px(86.0f);
    ImGui::SetCursorScreenPos(ImVec2(cx - logo_w * 0.5f, origin.y + Px(20.0f)));
    if (logo_.id)
        ImGui::Image(ImTextureRef(logo_.id), ImVec2(logo_w, logo_h));
    else
        ImGui::Dummy(ImVec2(logo_w, logo_h));
    const float title_y = origin.y + Px(20.0f) + logo_h + Px(10.0f);
    SpacedTitle(d, cx, title_y, "STRANDED", 5.0f, 7u);
    SpacedTitle(d, cx, title_y + Px(26.0f), "TOGETHER", 5.0f, 19u);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, title_y + Px(66.0f)));

    // Navigation.
    static const Page kPages[ui::kPageCount] = {Page::play, Page::in_game, Page::tools, Page::debug, Page::about};
    for (Page page : kPages) {
        if (!ui::PageVisible(phase, page)) continue;
        const bool enabled = ui::PageEnabled(phase, page);
        const bool active = page == page_;
        const ImVec2 p(origin.x + Px(12.0f), ImGui::GetCursorScreenPos().y);
        const ImVec2 size(w - Px(24.0f), Px(40.0f));
        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(static_cast<int>(page));
        if (!enabled) ImGui::BeginDisabled();
        if (ImGui::InvisibleButton("##navitem", size) && enabled) page_ = page;
        const bool hovered = enabled && ImGui::IsItemHovered();
        if (!enabled) ImGui::EndDisabled();
        ImGui::PopID();
        if (hovered && !active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 q(p.x + size.x, p.y + size.y);
        if (active) {
            d->AddRectFilled(p, q, col::kCard, Px(8.0f));
            d->AddRectFilled(ImVec2(p.x, p.y + Px(9.0f)), ImVec2(p.x + Px(3.0f), q.y - Px(9.0f)), col::kAccent, Px(2.0f));
        } else if (hovered) {
            d->AddRectFilled(p, q, col::Rgb(0x14, 0x1A, 0x20), Px(8.0f));
        }
        const ImU32 fg = !enabled ? col::kTextFaint : active ? col::kText : col::kTextDim;
        DrawIcon(d, PageIcon(page), ImVec2(p.x + Px(24.0f), p.y + size.y * 0.5f), Px(17.0f), active ? col::kAccent : fg);
        {
            const Font f(active ? GetFonts().semibold : GetFonts().regular, kBody);
            const float th = ImGui::GetTextLineHeight();
            d->AddText(ImVec2(p.x + Px(44.0f), p.y + (size.y - th) * 0.5f), fg, ui::PageLabel(page));
        }
        if (page == Page::in_game && ui::SessionActive(phase) && !active)
            StatusDot(ImVec2(q.x - Px(14.0f), p.y + size.y * 0.5f), 3.5f, link_.connected ? col::kGood : col::kCool);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, q.y + Px(4.0f)));
    }
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    // Status and version at the bottom.
    ImU32 status_color = col::kTextDim;
    const char* status = PhaseStatus(phase, link_, model_.session_role(), status_color);
    const float bottom = origin.y + h - Px(58.0f);
    d->AddLine(ImVec2(origin.x + Px(16.0f), bottom - Px(12.0f)), ImVec2(origin.x + w - Px(16.0f), bottom - Px(12.0f)), col::kBorder);
    StatusDot(ImVec2(origin.x + Px(26.0f), bottom + Px(9.0f)), 4.0f, status_color);
    {
        const Font f(GetFonts().semibold, kSmall);
        d->AddText(ImVec2(origin.x + Px(40.0f), bottom), col::kText, status);
    }
    {
        const Font f(GetFonts().regular, kLabel);
        const std::string version = "Version " + model_.version();
        d->AddText(ImVec2(origin.x + Px(40.0f), bottom + Px(20.0f)), col::kTextFaint, version.c_str());
    }
    ImGui::EndChild();
}

void LauncherUi::PageHeader(const char* title, const char* subtitle) {
    Text(title, col::kText, kTitle, GetFonts().display, -1.0f);
    if (subtitle && *subtitle) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        Text(subtitle, col::kTextDim, kBody, GetFonts().regular, 0.0f);
    }
    Gap(4.0f);
}

void LauncherUi::ResultLine(Result& result) {
    if (result.text.empty()) return;
    if (result.ok && !result.path.empty()) {
        const float button_w = 136.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 start = ImGui::GetCursorPos();
        Message(Icon::check, col::kGood, result.text, kSmall, avail - Px(button_w + 12.0f));
        const ImVec2 after = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(start.x + avail - Px(button_w), start.y - Px(8.0f)));
        if (Button("Show in folder##result", ButtonKind::quiet, ImVec2(button_w, 32.0f), true, Icon::folder, true))
            if (shell_.reveal_file) shell_.reveal_file(result.path);
        ImGui::SetCursorPos(ImVec2(start.x, std::fmax(after.y, start.y + Px(26.0f))));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        return;
    }
    Message(result.ok ? Icon::check : Icon::warning, result.ok ? col::kGood : col::kBad, result.text);
}

// The install gate in one row: game, mod, Steam.
void LauncherUi::ChecksStrip(const InstallChecks& c, Phase phase) {
    (void)phase;
    Chip(c.version_tested ? "Game found" : "Game: untested version", c.version_tested ? Icon::check : Icon::warning,
         c.version_tested ? col::kGood : col::kWarn, c.version_tested ? col::kGoodSoft : col::kWarnSoft);
    ImGui::SameLine(0.0f, Px(8.0f));
    {
        const ui::Tone tone = ui::ModTone(c.mod_state);
        const std::string label = ui::ModChipText(c);
        const Icon icon = tone == ui::Tone::good ? Icon::check : tone == ui::Tone::bad ? Icon::cross : tone == ui::Tone::warn ? Icon::warning : Icon::tools;
        Chip(label.c_str(), icon, tone == ui::Tone::neutral ? col::kCool : ToneColor(tone), SoftOf(tone));
    }
    ImGui::SameLine(0.0f, Px(8.0f));
    Chip(c.steam_running ? "Steam running" : "Steam not running", c.steam_running ? Icon::check : Icon::warning,
         c.steam_running ? col::kGood : col::kWarn, c.steam_running ? col::kGoodSoft : col::kWarnSoft);
    if (!c.version_tested && !c.version_note.empty()) {
        Gap(2.0f);
        BeginCard("##untested", 0.0f, col::kCard, col::kWarnBorder);
        IconBadge(Icon::warning, col::kWarn, col::kWarnSoft, 28.0f);
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::BeginGroup();
        Text("Untested game version", col::kText, kBody, GetFonts().semibold, -1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        Text(c.version_note.c_str(), col::kTextDim, kSmall, nullptr, 0.0f);
        ImGui::EndGroup();
        EndCard();
    }
    if (c.mod_state == ModState::mismatch && !c.mod_note.empty()) {
        Gap(2.0f);
        BeginCard("##mismatch", 0.0f, col::kCard, col::kWarnBorder);
        IconBadge(Icon::warning, col::kWarn, col::kWarnSoft, 28.0f);
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::BeginGroup();
        Text("The mod files don't match", col::kText, kBody, GetFonts().semibold, -1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        Text(c.mod_note.c_str(), col::kTextDim, kSmall, nullptr, 0.0f);
        ImGui::EndGroup();
        EndCard();
    } else if (c.mod_state == ModState::dev_build) {
        Gap(2.0f);
        Message(Icon::tools, col::kCool, "Development build: winhttp.dll is not checked against a release package.", kSmall, 0.0f, col::kTextDim);
    }
    Gap(4.0f);
}

// ---- Wrong folder and not ready ----------------------------------------------
void LauncherUi::WrongFolderPage() {
    PageHeader("Move me to your game folder",
               "Stranded Together has to be in the same folder as DEATH STRANDING DIRECTOR'S CUT (the folder with ds.exe).");
    const std::string here = model_.game_dir();
    const std::string found = model_.discovered_game_dir();
    BeginCard("##where", 0.0f, col::kCard, col::kBadBorder);
    SectionLabel("THIS COPY IS IN");
    Text(here.c_str(), col::kTextDim, kSmall, GetFonts().mono, 0.0f);
    EndCard();
    Gap(4.0f);
    BeginCard("##how");
    SectionLabel("HOW TO FIX IT");
    Text("1.  Open your DEATH STRANDING DIRECTOR'S CUT folder.", col::kText, kBody, nullptr, 0.0f);
    Text("2.  Extract everything from the Stranded Together zip into it (StrandedTogether.exe and winhttp.dll go next to ds.exe).",
         col::kText, kBody, nullptr, 0.0f);
    Text("3.  Start StrandedTogether.exe from there.", col::kText, kBody, nullptr, 0.0f);
    Gap(4.0f);
    if (!found.empty()) {
        SectionLabel("STEAM HAS THE GAME HERE");
        Text(found.c_str(), col::kTextDim, kSmall, GetFonts().mono, 0.0f);
    } else {
        Text("In Steam, right-click DEATH STRANDING DIRECTOR'S CUT, then Manage > Browse local files.", col::kTextDim, kSmall, nullptr, 0.0f);
    }
    EndCard();
    Gap(6.0f);
    if (!found.empty()) {
        if (Button("Open my game folder", ButtonKind::primary, ImVec2(0, 40.0f), true, Icon::folder, true))
            if (shell_.open_folder) shell_.open_folder(found);
        ImGui::SameLine();
    }
    if (Button("Check again", found.empty() ? ButtonKind::primary : ButtonKind::secondary, ImVec2(0, 40.0f))) model_.recheck();
}

// Phase::error on Play: why co-op cannot start here, and what to do.
void LauncherUi::NotReadyCard() {
    const InstallChecks c = model_.checks();
    const bool foreign = c.mod_state == ModState::foreign;
    const bool disabled = c.mod_state == ModState::disabled;
    const bool missing = c.mod_state == ModState::missing;
    BeginCard("##notready", 0.0f, col::kCard, disabled ? col::kWarnBorder : col::kBadBorder);
    IconBadge(missing ? Icon::cable : disabled ? Icon::stop : Icon::warning, disabled ? col::kWarn : col::kBad,
              disabled ? col::kWarnSoft : col::kBadSoft, 36.0f);
    ImGui::SameLine(0.0f, Px(12.0f));
    ImGui::BeginGroup();
    const char* title = c.mod_present ? "Co-op can't start here"
                        : foreign     ? "Another winhttp.dll is in the way"
                        : disabled    ? "The mod is switched off"
                                      : "Install the co-op mod first";
    Text(title, col::kText, kHeading, GetFonts().semibold, -1.0f);
    Text(model_.last_error().c_str(), col::kTextDim, kBody, nullptr, 0.0f);
    ImGui::EndGroup();
    if (missing) {
        Gap(2.0f);
        SectionLabel("HOW TO INSTALL");
        Text("1.  Open the game folder (button below).", col::kText, kSmall, nullptr, 0.0f);
        Text("2.  Extract everything from the Stranded Together zip into it: StrandedTogether.exe, winhttp.dll and the "
             "stranded_together folder go next to ds.exe.",
             col::kText, kSmall, nullptr, 0.0f);
        Text("3.  Press Check again. Without this launcher the game still starts unmodded.", col::kText, kSmall, nullptr, 0.0f);
    } else if (foreign) {
        Gap(2.0f);
        SectionLabel("HOW TO FIX IT");
        Text("1.  Quit the game. Open the game folder (button below).", col::kText, kSmall, nullptr, 0.0f);
        Text("2.  Move winhttp.dll somewhere else: it belongs to another mod or tool.", col::kText, kSmall, nullptr, 0.0f);
        Text("3.  Extract the Stranded Together zip into the game folder again, then press Check again.", col::kText, kSmall, nullptr, 0.0f);
    }
    Gap(4.0f);
    if (disabled) {
        ModSwitchButton(c, true, 0.0f);
    } else if (Button("Check again", ButtonKind::primary, ImVec2(0, 40.0f))) {
        model_.recheck();
    }
    ImGui::SameLine();
    if (Button("Open game folder", ButtonKind::secondary, ImVec2(0, 40.0f), true, Icon::folder, true))
        if (shell_.open_folder) shell_.open_folder(model_.game_dir());
    ImGui::SameLine();
    if (Button("Collect logs", ButtonKind::quiet, ImVec2(0, 40.0f), true, Icon::logs, true)) {
        std::string path, error;
        const bool ok = model_.collect_logs(path, error);
        logs_result_ = Result{ok, ok ? "Saved to your Desktop: " + FileName(path) : error, path};
    }
    ResultLine(logs_result_);
    if (disabled) ResultLine(debug_result_);
    EndCard();
}

// ---- Play ------------------------------------------------------------------
void LauncherUi::PlayPage(Phase phase) {
    PageHeader("Play together", "Host a game for your partner, join theirs, or try the mod on your own.");
    UpdateCard(phase);
    const InstallChecks checks = model_.checks();
    ChecksStrip(checks, phase);
    if (phase == Phase::error) {
        NotReadyCard();
        return;
    }
    const bool session = ui::SessionActive(phase);
    ui::PlayTab tab = play_tab_;
    if (session) tab = ui::TabForRole(model_.session_role());
    static const char* const kTabs[ui::kPlayTabCount] = {ui::PlayTabLabel(ui::PlayTab::host), ui::PlayTabLabel(ui::PlayTab::join),
                                                          ui::PlayTabLabel(ui::PlayTab::solo)};
    int index = static_cast<int>(tab);
    if (Segmented("##role", kTabs, ui::kPlayTabCount, &index, 360.0f, !session)) {
        play_tab_ = static_cast<ui::PlayTab>(index);
        play_error_.clear();
    }
    Gap(4.0f);
    switch (tab) {
    case ui::PlayTab::host: HostTab(phase, checks); break;
    case ui::PlayTab::join: JoinTab(phase); break;
    case ui::PlayTab::solo: SoloTab(phase); break;
    }
}

void LauncherUi::CodeCard(bool compact) {
    const ui::CodeDisplay code = ui::SplitJoinCode(hosted_code_);
    BeginCard(compact ? "##codecompact" : "##code", 0.0f, col::kCard, col::kAccentBorder);
    SectionLabel("YOUR JOIN CODE");
    const bool copied = copied_until_ > time_;
    if (compact) {
        const float button_w = 112.0f;
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();
        Text(code.main.c_str(), col::kText, 17.0f, GetFonts().mono, -1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        Text(code.account.c_str(), col::kTextDim, kSmall, GetFonts().mono, -1.0f);
        ImGui::EndGroup();
        ImGui::SameLine(avail - Px(button_w) + ImGui::GetStyle().WindowPadding.x);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(4.0f));
        if (Button(copied ? "Copied##copy" : "Copy##copy", ButtonKind::secondary, ImVec2(button_w, 36.0f), true, copied ? Icon::check : Icon::copy,
                   true))
            CopyCode();
    } else {
        Text(code.main.c_str(), col::kAccent, kCode, GetFonts().mono, -1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        Text(code.account.c_str(), col::kTextDim, kBody, GetFonts().mono, -1.0f);
        Gap(4.0f);
        if (Button(copied ? "Copied##copy" : "Copy code##copy", ButtonKind::primary, ImVec2(132.0f, 38.0f), true, copied ? Icon::check : Icon::copy,
                   true))
            CopyCode();
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(2.0f));
        Text(copied ? "It's on your clipboard. Paste it to your partner in Steam chat or Discord."
                    : "Send it to your partner in Steam chat or Discord.",
             col::kTextDim, kSmall, nullptr, 0.0f);
    }
    EndCard();
}

// While a session from this tab runs: launching (spinner + Cancel) or in the game.
void LauncherUi::SessionNote(Phase phase) {
    if (phase == Phase::launching) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Spinner(ImVec2(p.x + Px(10.0f), p.y + Px(17.0f)), 7.0f, col::kAccent, time_);
        ImGui::SetCursorScreenPos(ImVec2(p.x + Px(28.0f), p.y + Px(8.0f)));
        Text("Starting DEATH STRANDING through Steam...", col::kTextDim, kBody, nullptr, -1.0f);
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
        if (Button("Cancel##launch", ButtonKind::quiet, ImVec2(0, 34.0f), true, Icon::stop, true)) {
            std::string error;
            if (!model_.cancel_launch(error)) play_error_ = error;
        }
    } else {
        Message(Icon::signal, col::kCool, phase == Phase::after_game ? "The game has closed." : "You're in the game.", kBody, 0.0f, col::kText);
        // What the running game reports off (the mod's link-status.json "features_off"): never silent.
        if (phase == Phase::in_game && !model_.link().features_off.empty())
            Message(Icon::warning, col::kWarn, "Off in this game: " + model_.link().features_off, kSmall, 0.0f, col::kText);
        if (Button("See the connection", ButtonKind::quiet, ImVec2(0, 34.0f), true, Icon::signal, true)) Navigate(Page::in_game);
    }
    Message(Icon::warning, col::kBad, play_error_);
}

void LauncherUi::HostTab(Phase phase, const InstallChecks& checks) {
    const bool session = ui::SessionActive(phase);
    if (session && model_.session_role() == Role::host) {
        if (!hosted_code_.empty()) {
            CodeCard(false);
            Gap(2.0f);
        }
        SessionNote(phase);
        return;
    }
    BeginCard("##hostinfo");
    IconBadge(Icon::cable, col::kAccent, col::kAccentSoft, 36.0f);
    ImGui::SameLine(0.0f, Px(12.0f));
    ImGui::BeginGroup();
    Text("You host, your partner joins", col::kText, kBody, GetFonts().semibold, -1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
    Text("The game starts through Steam and you get a join code for your partner. Load your save; they appear in your world once "
         "connected.",
         col::kTextDim, kSmall, nullptr, 0.0f);
    Gap(2.0f);
    // The guest picks a character on Join; the host has no choice (my_character=sam).
    Message(Icon::person, col::kAccent, "You appear as: Sam (the host is always Sam)", kSmall, 0.0f, col::kText);
    // The host-held save (M1.11): said plainly before the host starts.
    if (model_.debug_options().host_save)
        Message(Icon::info, col::kCool,
                model_.debug_options().backup_host_saves
                    ? "Co-op saves into your own save: it holds the session's progress. Your partner waits until you pick Continue or "
                      "New Game. A backup of your saves is made first (stranded_together\\backups, the newest 5 kept)."
                    : "Co-op saves into your own save: it holds the session's progress. Your partner waits until you pick Continue or "
                      "New Game. No backup is made (Debug page).",
                kSmall, 0.0f, col::kTextDim);
    else
        Message(Icon::warning, col::kWarn, kHostSaveOffNote, kSmall, 0.0f, col::kTextDim);
    // partner_body=holo installs no pose hook, so this game sends no joint frames: the partner
    // sees a hologram whatever this game's character is (docs/real-partner-body.md section 5).
    if (!model_.debug_options().real_partner_body)
        Message(Icon::warning, col::kWarn, kHologramNote, kSmall, 0.0f, col::kTextDim);
    ImGui::EndGroup();
    EndCard();
    Gap(2.0f);
    const bool can = ui::CanHost(phase, checks);
    if (Button("Host game", ButtonKind::primary, ImVec2(190.0f, 46.0f), can, Icon::play, true)) {
        CommitFields();
        std::string code, error;
        if (model_.host(code, error)) {
            hosted_code_ = code;
            play_error_.clear();
            CopyCode();
        } else {
            play_error_ = error;
        }
    }
    ImGui::SameLine(0.0f, Px(14.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(12.0f));
    if (!checks.steam_running)
        Message(Icon::warning, col::kWarn, "Start Steam and log in to host: the code carries your Steam account.", kSmall, 0.0f, col::kTextDim);
    else
        Message(Icon::steam, col::kCool,
                model_.transport() == Transport::udp ? "Connects over your LAN / VPN (Advanced)." : "Connects through Steam.", kSmall, 0.0f,
                col::kTextDim);
    Message(Icon::warning, col::kBad, play_error_);
    Gap(4.0f);
    AdvancedSection(session);
}

void LauncherUi::JoinTab(Phase phase) {
    const bool session = ui::SessionActive(phase);
    if (session && model_.session_role() == Role::join) {
        BeginCard("##joined");
        IconBadge(Icon::cable, col::kCool, col::kCoolSoft, 36.0f);
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::BeginGroup();
        Text("Joining your partner", col::kText, kBody, GetFonts().semibold, -1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
        Text(model_.session_transport() == Transport::udp ? "Over your LAN / VPN." : "Through Steam.", col::kTextDim, kSmall, nullptr, 0.0f);
        ImGui::EndGroup();
        EndCard();
        Gap(2.0f);
        SessionNote(phase);
        return;
    }
    SectionLabel("JOIN CODE");
    const float avail = ImGui::GetContentRegionAvail().x;
    const float field_w = std::min(avail, Px(470.0f));
    const std::string text(code_buf_);
    if (text != checked_text_) {
        checked_text_ = text;
        checked_ = model_.check_code(text);
    }
    const ui::JoinFieldView view = ui::EvaluateJoinField(text, checked_, session);
    // "Paste" sits at the right end of the label row.
    const float paste_w = 82.0f;
    ImGui::SameLine(ImGui::GetCursorStartPos().x + field_w - Px(paste_w));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(9.0f));
    const bool paste = Button("Paste##code", ButtonKind::quiet, ImVec2(paste_w, 28.0f), view.field_enabled, Icon::paste, true);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    {
        const Font f(GetFonts().mono, kBody);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(12.0f, 11.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, view.tone == ui::Tone::bad    ? col::kBadBorder
                                               : view.tone == ui::Tone::good ? col::kGoodBorder
                                                                             : col::kBorder);
        if (!view.field_enabled) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(field_w);
        const bool enter = ImGui::InputText("##code", code_buf_, sizeof(code_buf_),
                                            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool active = ImGui::IsItemActive();
        const ImVec2 field_min = ImGui::GetItemRectMin();
        if (!view.field_enabled) ImGui::EndDisabled();
        if (code_buf_[0] == 0 && !active) {
            ImGui::GetWindowDrawList()->AddText(GetFonts().mono, ImGui::GetFontSize(), ImVec2(field_min.x + Px(12.0f), field_min.y + Px(11.0f)),
                                                col::kTextFaint, "DST1-XXXXX-XXXXX-XXXXX-XXXXX-XX@7656...");
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (enter && view.can_join && ui::CanStartSession(phase)) {
            CommitFields();
            std::string error;
            if (!model_.join(text, error)) play_error_ = error;
            else play_error_.clear();
        }
    }
    if (paste) {
        if (const char* clip = ImGui::GetClipboardText()) {
            std::string trimmed(clip);
            while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r' || trimmed.back() == ' ')) trimmed.pop_back();
            strncpy_s(code_buf_, sizeof(code_buf_), trimmed.c_str(), _TRUNCATE);
        }
    }
    {
        const Icon icon = view.tone == ui::Tone::good ? Icon::check : view.tone == ui::Tone::bad ? Icon::warning : Icon::info;
        Message(icon, ToneColor(view.tone), view.message, kSmall, field_w);
    }
    Gap(4.0f);
    // The character the host sees you as (session.ini my_character). Saved when picked.
    SectionLabel("YOUR CHARACTER");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    {
        std::string character = model_.my_character();
        const float row_y = ImGui::GetCursorPosY();
        if (CharacterCombo("##character", &character, 220.0f, view.field_enabled, model_.debug_options().allow_other_characters,
                           open_character_list_))
            model_.set_my_character(character);
        open_character_list_ = false;
        ImGui::SameLine(0.0f, Px(14.0f));
        ImGui::SetCursorPosY(row_y + Px(1.0f));
        if (model_.debug_options().real_partner_body)
            Text(character == core::kHostCharacter        ? "Your partner sees you as this character (experimental)."
                 : model_.debug_options().self_character ? "Your partner sees you as this character (experimental),\nand so do you on your own screen."
                                                         : "Your partner sees you as this character (experimental).\nOn your own screen you stay Sam (Debug page).",
                 col::kTextDim, kSmall, nullptr, 0.0f);
        else
            Text("Partner body is Hologram (Debug page): your partner\nsees you as a hologram, not this character.", col::kWarn, kSmall,
                 nullptr, 0.0f);
    }
    if (model_.debug_options().host_save) {
        Gap(2.0f);
        // M1.11: what the guest's progress is.
        Message(Icon::info, col::kCool,
                "You play in your partner's world: your game loads a copy of their save once they start, and shows a waiting screen "
                "until then. Progress you make there (orders, deliveries, equipment) is a disposable copy and is not kept; your own "
                "saves are never opened. Your settings and key bindings from co-op are kept for the next co-op game.",
                kSmall, 0.0f, col::kTextDim);
    } else {
        Gap(2.0f);
        Message(Icon::warning, col::kWarn, kHostSaveOffNote, kSmall, 0.0f, col::kTextDim);
    }
    Gap(6.0f);
    if (Button("Join game", ButtonKind::primary, ImVec2(190.0f, 46.0f), view.can_join && ui::CanStartSession(phase), Icon::play, true)) {
        CommitFields();
        std::string error;
        if (!model_.join(text, error)) play_error_ = error;
        else play_error_.clear();
    }
    ImGui::SameLine(0.0f, Px(14.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(12.0f));
    Message(Icon::steam, col::kCool,
            model_.transport() == Transport::udp ? "Connects over your LAN / VPN (Advanced)." : "Connects through Steam.", kSmall, 0.0f,
            col::kTextDim);
    Message(Icon::warning, col::kBad, play_error_);
    Gap(4.0f);
    AdvancedSection(session);
}

void LauncherUi::SoloTab(Phase phase) {
    const bool session = ui::SessionActive(phase);
    BeginCard("##soloinfo", 0.0f, col::kCard, col::kCoolBorder);
    IconBadge(Icon::echo, col::kCool, col::kCoolSoft, 36.0f);
    ImGui::SameLine(0.0f, Px(12.0f));
    ImGui::BeginGroup();
    Text("Solo test (echo)", col::kText, kBody, GetFonts().semibold, -1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(6.0f));
    Text("Starts the game with an echo: a copy of your own character that replays your movement about two seconds behind you. No "
         "partner and no network needed. Use it to check the mod works on your PC.",
         col::kTextDim, kSmall, nullptr, 0.0f);
    ImGui::EndGroup();
    EndCard();
    Gap(2.0f);
    if (session && (model_.session_role() == Role::echo || model_.session_role() == Role::solo)) {
        SessionNote(phase);
        return;
    }
    if (Button("Start solo test", ButtonKind::primary, ImVec2(210.0f, 46.0f), ui::CanStartSession(phase), Icon::echo, true)) {
        CommitFields();
        std::string error;
        if (!model_.solo_test(error)) play_error_ = error;
        else play_error_.clear();
    }
    ImGui::SameLine(0.0f, Px(12.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(6.0f));
    if (Button("Debug options##solo", ButtonKind::quiet, ImVec2(0, 34.0f), true, Icon::sliders, true)) {
        debug_tab_ = ui::DebugTab::options;
        Navigate(Page::debug);
    }
    Gap(2.0f);
    Text("Nothing is sent anywhere: the echo runs inside your own game. The Debug page sets its delay and offset, and starts the "
         "debug tools on their own.",
         col::kTextFaint, kSmall, nullptr, 0.0f);
    Message(Icon::warning, col::kBad, play_error_);
}

// Advanced: the transport (Steam or LAN/VPN over UDP), the LAN addresses and the display name.
void LauncherUi::AdvancedSection(bool session) {
    const bool udp = model_.transport() == Transport::udp;
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (Button("Advanced: connection and name##adv", ButtonKind::quiet, ImVec2(0, 30.0f), true, Icon::sliders, true))
            advanced_open_ = !advanced_open_;
        if (!advanced_open_ && udp) {
            ImGui::SameLine(0.0f, Px(6.0f));
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y + Px(1.0f)));
            Chip("LAN mode on", Icon::network, col::kCool, col::kCoolSoft);
        }
    }
    if (!advanced_open_) return;
    BeginCard("##advanced", 0.0f, col::kCard, udp ? col::kCoolBorder : col::kBorder);
    SectionLabel("CONNECTION");
    static const char* const kModes[2] = {"Steam", "LAN / VPN (UDP)"};
    int mode = udp ? 1 : 0;
    if (Segmented("##transport", kModes, 2, &mode, 300.0f, !session)) model_.set_transport(mode == 1 ? Transport::udp : Transport::steam);
    if (mode == 1) {
        Gap(2.0f);
        // Saved when a field loses focus (and before Host / Join), not on every keystroke.
        bool edited = false;
        Field("##ip", "PARTNER'S IP ADDRESS", ip_buf_, sizeof(ip_buf_), 170.0f, !session, "192.168.1.20");
        if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
        ImGui::SameLine(0.0f, Px(12.0f));
        Field("##plocal", "YOUR PORT", port_local_buf_, sizeof(port_local_buf_), 90.0f, !session, nullptr, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
        ImGui::SameLine(0.0f, Px(12.0f));
        Field("##ppeer", "PARTNER'S PORT", port_peer_buf_, sizeof(port_peer_buf_), 90.0f, !session, nullptr, ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
        if (edited) CommitFields();
        LanSettings typed;
        typed.peer_ip = ip_buf_;
        int port = 0;
        typed.port_local = ui::ParsePort(port_local_buf_, port) ? port : 0;
        typed.port_peer = ui::ParsePort(port_peer_buf_, port) ? port : 0;
        const std::string problem = ui::LanProblem(typed);
        if (!problem.empty())
            Message(Icon::warning, col::kWarn, problem);
        else
            Message(Icon::info, col::kTextFaint,
                    "Both PCs need the same join code (the guest still pastes the host's code) and must allow UDP on these ports. "
                    "Testing two copies on one PC: swap the ports on the second one.");
    }
    Gap(4.0f);
    Field("##name", "YOUR NAME IN CO-OP (OPTIONAL)", name_buf_, sizeof(name_buf_), 220.0f, !session, "your Steam name");
    if (ImGui::IsItemDeactivatedAfterEdit()) CommitFields();
    EndCard();
}

// ---- In game ---------------------------------------------------------------
void LauncherUi::InGamePage(Phase phase) {
    if (!ui::SessionActive(phase)) {
        PageHeader("In game", "No game is running.");
        Text("Host, join or start a solo test on the Play page. Its connection shows up here.", col::kTextDim, kBody, nullptr, 0.0f);
        Gap(4.0f);
        if (Button("Go to Play", ButtonKind::primary, ImVec2(0, 40.0f), true, Icon::play, true)) Navigate(Page::play);
        return;
    }
    const Role role = model_.session_role();
    const bool host = role == Role::host;
    const bool solo = role == Role::solo;
    const bool echo = role == Role::echo || solo;
    PageHeader("In game", solo   ? "Debug tools only: no partner, no network."
                          : echo ? "A solo test with your echo."
                          : host ? "You're hosting. Your partner joins you."
                                 : "You're joining your partner's game.");
    const LinkView& l = link_;
    const bool busy = phase == Phase::launching;
    const ImU32 tone = phase == Phase::after_game ? col::kTextDim : LinkToneColor(l.tone);
    const ImU32 border = l.tone == LinkView::Tone::good  ? col::kGoodBorder
                         : l.tone == LinkView::Tone::warn ? col::kWarnBorder
                         : l.tone == LinkView::Tone::bad  ? col::kBadBorder
                                                          : col::kBorder;
    BeginCard("##status", 0.0f, col::kCard, border);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 c(p.x + Px(17.0f), p.y + Px(17.0f));
        if (busy)
            Spinner(c, 10.0f, col::kAccent, time_);
        else if (phase == Phase::after_game)
            DrawIcon(ImGui::GetWindowDrawList(), Icon::check, c, Px(26.0f), col::kGood);
        else
            StatusDot(c, 6.5f, tone);
        ImGui::Dummy(Px(34.0f, 34.0f));
    }
    ImGui::SameLine(0.0f, Px(12.0f));
    ImGui::BeginGroup();
    Text(l.headline.empty() ? "Waiting for the game..." : l.headline.c_str(), col::kText, kHeading, GetFonts().semibold, -1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    Text(l.detail.c_str(), col::kTextDim, kBody, nullptr, 0.0f);
    if (!l.features_off.empty() && phase == Phase::in_game)
        Message(Icon::warning, col::kWarn, "Off in this game: " + l.features_off, kSmall, 0.0f, col::kText);
    ImGui::EndGroup();
    if (phase == Phase::launching) {
        Gap(2.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(46.0f));
        if (Button("Cancel##launch", ButtonKind::secondary, ImVec2(0, 34.0f), true, Icon::stop, true)) {
            std::string error;
            if (!model_.cancel_launch(error)) play_error_ = error;
        }
    }
    EndCard();

    if (phase == Phase::in_game) {
        Gap(2.0f);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap = Px(10.0f);
        const float tile_w = (avail - 2 * gap) / 3.0f;
        struct Tile {
            const char* label;
            std::string value;
            ImU32 color;
        };
        Tile tiles[3];
        const DebugOptions dbg = model_.debug_options();
        if (solo) {
            tiles[0] = {"MODE", "Debug tools", col::kText};
            tiles[1] = {"DEBUG MENU", dbg.debug_menu ? "Press F" + std::to_string(dbg.debug_menu_key) : std::string("Off"),
                        dbg.debug_menu ? col::kAccent : col::kTextFaint};
            tiles[2] = {"NETWORK", "None (local)", col::kText};
        } else if (echo) {
            char delay[32];
            std::snprintf(delay, sizeof(delay), "%.1f s behind", dbg.echo_delay_s);
            tiles[0] = {"ECHO", ui::RemoteText(l.remote), l.remote == "visible" ? col::kGood : col::kTextFaint};
            tiles[1] = {"DELAY", delay, col::kText};
            tiles[2] = {"NETWORK", "None (local)", col::kText};
        } else {
            const std::string partner = l.connected && !l.peer.empty() ? l.peer
                                        : !l.state.empty() && !l.fresh ? std::string("Unknown")
                                        : l.state == "lost"            ? std::string("Left the game")
                                                                       : std::string("Not here yet");
            tiles[0] = {"PARTNER", partner, l.connected ? col::kText : col::kTextFaint};
            tiles[1] = {"PING", ui::FormatPing(l.ping_ms), l.ping_ms > 0 ? ToneColor(ui::PingTone(l.ping_ms)) : col::kTextFaint};
            tiles[2] = {"CONNECTION", l.state.empty() ? std::string("-") : ui::RouteText(l.transport, l.route),
                        l.state.empty() ? col::kTextFaint : col::kCool};
        }
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0.0f, gap);
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, col::kCard);
            ImGui::PushStyleColor(ImGuiCol_Border, col::kBorder);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(10.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(14.0f, 11.0f));
            ImGui::BeginChild("##tile", ImVec2(tile_w, Px(66.0f)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            SectionLabel(tiles[i].label);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
            Text(tiles[i].value.c_str(), tiles[i].color, 17.0f, GetFonts().semibold, -1.0f);
            ImGui::EndChild();
            ImGui::PopID();
        }
        if (!echo && l.connected) {
            Gap(2.0f);
            Message(Icon::person, l.remote == "visible" ? col::kGood : col::kCool, "Your partner's porter: " + ui::RemoteText(l.remote), kSmall, 0.0f,
                    col::kTextDim);
        }
    }

    // The code matters until the partner is in (and again if they leave).
    if (host && !hosted_code_.empty() && phase != Phase::after_game && !l.connected) {
        Gap(2.0f);
        CodeCard(true);
    }
    Message(Icon::warning, col::kBad, play_error_);
    Gap(4.0f);
    Message(Icon::info, col::kTextFaint,
            phase == Phase::after_game ? "You can close this window."
                                       : "You can close this window. Co-op keeps running and switches itself off after the game closes.");
}

// ---- Tools -----------------------------------------------------------------
void LauncherUi::ToolsPage(Phase phase) {
    PageHeader("Tools", nullptr);
    const bool in_game = phase == Phase::in_game || phase == Phase::launching;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = Px(12.0f);
    const float card_w = std::floor((avail - gap) * 0.5f);

    auto card = [&](const char* id, Icon icon, ImU32 icon_color, ImU32 icon_bg, const char* title, const char* description,
                    float logical_height, auto&& body) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col::kCard);
        ImGui::PushStyleColor(ImGuiCol_Border, col::kBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(16.0f, 14.0f));
        ImGui::BeginChild(id, ImVec2(card_w, Px(logical_height)), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Px(8.0f, 8.0f));
        IconBadge(icon, icon_color, icon_bg, 30.0f);
        ImGui::SameLine(0.0f, Px(10.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(4.0f));
        Text(title, col::kText, kBody + 1.0f, GetFonts().semibold, -1.0f);
        Text(description, col::kTextDim, kSmall, nullptr, 0.0f);
        body();
        ImGui::PopStyleVar();
        ImGui::EndChild();
    };
    auto bottom = [&]() {
        const float y = ImGui::GetWindowHeight() - Px(14.0f) - Px(34.0f);
        if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
    };
    const float h = 158.0f;
    card("##modfolder", Icon::folder, col::kAccent, col::kAccentSoft, "Mod folder",
         "The stranded_together folder: session settings, status and logs.", h, [&] {
             bottom();
             if (Button("Open folder##mod", ButtonKind::secondary, ImVec2(-1.0f, 34.0f), true, Icon::folder, true))
                 if (shell_.open_folder) shell_.open_folder(model_.mod_dir());
         });
    ImGui::SameLine(0.0f, gap);
    card("##modlog", Icon::logs, col::kCool, col::kCoolSoft, "Mod log", "What the co-op mod did in the game: stranded_together.log.", h, [&] {
        bottom();
        if (Button("Open log##log", ButtonKind::secondary, ImVec2(-1.0f, 34.0f), true, Icon::logs, true))
            if (shell_.open_file) shell_.open_file(model_.mod_log_path());
    });
    Gap(2.0f);
    card("##collect", Icon::download, col::kTextDim, col::Rgb(0x9A, 0xA6, 0xB1, 0x22), "Collect logs",
         "A zip on your Desktop for a bug report. Join codes, secrets and Steam IDs are removed.", h, [&] {
             bottom();
             if (Button("Collect logs##logs", ButtonKind::secondary, ImVec2(-1.0f, 34.0f))) {
                 std::string path, error;
                 const bool ok = model_.collect_logs(path, error);
                 tool_result_ = Result{ok, ok ? "Saved to your Desktop: " + FileName(path) + "." : error, path};
             }
         });
    ImGui::SameLine(0.0f, gap);
    card("##end", Icon::stop, col::kBad, col::kBadSoft, "End session",
         "Switches co-op off now (removes session.ini), so the game starts unmodded. Not while the game runs.", h, [&] {
             bottom();
             if (Button("End session##end", ButtonKind::secondary, ImVec2(-1.0f, 34.0f), !in_game && model_.session_pending())) {
                 std::string error;
                 const bool ok = model_.end_session(error);
                 tool_result_ = Result{ok, ok ? "Co-op is switched off. The game starts unmodded until you host or join again." : error, ""};
             }
         });
    Gap(4.0f);
    if (in_game) {
        Message(Icon::info, col::kTextFaint, "A game is running: End session is available after it closes (co-op then switches itself off).");
        Gap(2.0f);
    }
    ResultLine(tool_result_);
    Gap(6.0f);
    SectionLabel("GAME FOLDER");
    Text(model_.game_dir().c_str(), col::kTextDim, kSmall, GetFonts().mono, 0.0f);
    Gap(2.0f);
    Message(Icon::sliders, col::kCool, "Test options, the live mod log and switching the mod off are on the Debug page.", kSmall, 0.0f,
            col::kTextFaint);
}

// ---- Debug -----------------------------------------------------------------
void LauncherUi::ModSwitchButton(const InstallChecks& c, bool enabled, float logical_width) {
    const bool off = c.mod_state == ModState::disabled;
    const bool can = enabled && (off || c.mod_present);
    const char* label = off ? "Enable mod##modswitch" : "Disable mod##modswitch";
    if (Button(label, off ? ButtonKind::primary : ButtonKind::secondary, ImVec2(logical_width, 40.0f), can, off ? Icon::play : Icon::stop, true)) {
        std::string error;
        const bool ok = model_.set_mod_enabled(off, error);
        debug_result_ = Result{ok,
                               ok ? (off ? "The mod is on again (winhttp.dll)." : "The mod is switched off (winhttp.dll.disabled): the game runs unmodded.")
                                  : error,
                               ""};
    }
}

void LauncherUi::RefreshLog(bool force) {
    if (!force && time_ - log_refreshed_at_ < 1.0) return;
    log_refreshed_at_ = time_;
    std::string text = ui::LastLines(model_.mod_log_tail(256u * 1024u), 400);
    if (text != log_text_) {
        log_text_ = std::move(text);
        if (log_follow_) log_scroll_end_ = true;
    }
}

void LauncherUi::DebugPage(Phase phase) {
    PageHeader("Debug", "Test tools for the mod. The options go into every game you start from this launcher.");
    static const char* const kTabs[ui::kDebugTabCount] = {ui::DebugTabLabel(ui::DebugTab::options), ui::DebugTabLabel(ui::DebugTab::saves),
                                                          ui::DebugTabLabel(ui::DebugTab::files)};
    int index = static_cast<int>(debug_tab_);
    if (Segmented("##debugtab", kTabs, ui::kDebugTabCount, &index, 420.0f, true)) {
        debug_tab_ = static_cast<ui::DebugTab>(index);
        debug_result_ = Result{};
        if (debug_tab_ == ui::DebugTab::files) RefreshLog(true);
    }
    Gap(4.0f);
    if (debug_tab_ == ui::DebugTab::options) DebugOptionsTab(phase);
    else if (debug_tab_ == ui::DebugTab::saves) DebugSavesTab(phase);
    else DebugFilesTab(phase);
}

// Debug > Test saves: a save the user picked once (e.g. a downloaded 100% save), played on a
// sandbox copy, and the local two-player test (two games on this PC).
void LauncherUi::DebugSavesTab(Phase phase) {
    const TestSaveView t = model_.test_save();
    const LocalTestView lt = model_.local_test();
    const bool can = ui::CanStartSession(phase) && !lt.running;
    const float avail = ImGui::GetContentRegionAvail().x / theme::Scale();
    BeginCard("##testsave");
    SectionLabel("TEST SAVE");
    {
        const Font f(GetFonts().mono, kSmall);
        Text(t.folder.empty() ? "No folder picked" : t.folder.c_str(), t.folder.empty() ? col::kTextFaint : col::kText, kSmall, GetFonts().mono,
             0.0f);
    }
    Message(t.folder_ok ? Icon::check : Icon::info, t.folder_ok ? col::kGood : col::kTextFaint, t.folder_note, kSmall);
    Gap(2.0f);
    const float w3 = std::floor((avail - 16.0f - 24.0f) / 3.0f);
    if (Button("Choose folder...##testsave", ButtonKind::secondary, ImVec2(w3, 36.0f), can && shell_.pick_folder != nullptr, Icon::folder, true)) {
        std::string folder;
        if (shell_.pick_folder(folder)) {
            std::string error;
            const bool ok = model_.set_test_save_folder(folder, error);
            debug_result_ = Result{ok, ok ? "Test save folder set. Your own saves are only read, never written." : error, ""};
        }
    }
    ImGui::SameLine(0.0f, Px(8.0f));
    if (Button("Open test-save folder", ButtonKind::quiet, ImVec2(w3, 36.0f), true, Icon::folder, true))
        if (shell_.open_folder) shell_.open_folder(t.sandbox_dir);
    ImGui::SameLine(0.0f, Px(8.0f));
    if (Button("Reset test saves", ButtonKind::quiet, ImVec2(w3, 36.0f), can && t.seeded, Icon::trash, true)) {
        std::string error;
        const bool ok = model_.reset_test_saves(error);
        debug_result_ = Result{ok, ok ? "The test copies are deleted; the next game starts again from the picked save." : error, ""};
    }
    Gap(2.0f);
    if (Button("Play with test save (your saves untouched)", ButtonKind::primary, ImVec2(318.0f, 40.0f), can && t.folder_ok, Icon::play, true)) {
        CommitFields();
        std::string error;
        const bool ok = model_.play_test_save(error);
        debug_result_ = Result{ok, ok ? "Starting DEATH STRANDING on the test save (debug tools only)." : error, ""};
    }
    ImGui::SameLine(0.0f, Px(14.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(9.0f));
    bool use = t.use_for_all;
    if (Checkbox("Also for Host, Join, Solo test", &use, t.folder_ok)) model_.set_use_test_save(use);
    Text("The game reads and writes only a copy in stranded_together\\sandbox (your profile copied in, the save's slots in place "
         "of yours). Your own save folder is only read, and hashed before and after.",
         col::kTextDim, kSmall, nullptr, 0.0f);
    if (!t.result.empty()) {
        const ImU32 c = t.tone == TestSaveView::Tone::good ? col::kGood : t.tone == TestSaveView::Tone::bad ? col::kBad
                        : t.tone == TestSaveView::Tone::warn ? col::kWarn : col::kTextDim;
        Message(t.tone == TestSaveView::Tone::good ? Icon::check : Icon::warning, c, t.result, kSmall);
    }
    EndCard();
    Gap(4.0f);

    BeginCard("##localtest", 0.0f, col::kCard, lt.running ? col::kCoolBorder : col::kBorder);
    SectionLabel("LOCAL TWO-PLAYER TEST");
    if (!lt.running)
        Text("Two games on this PC, linked over loopback: host window = keyboard + mouse, guest window = gamepad. Each runs on "
             "its own copy of the test save (or of your saves), at 30 fps.",
             col::kTextDim, kSmall, nullptr, 0.0f);
    Gap(2.0f);
    if (Button("Local two-player test", ButtonKind::primary, ImVec2(232.0f, 40.0f), can, Icon::cable, true)) {
        CommitFields();
        std::string error;
        const bool ok = model_.start_local_test(error);
        debug_result_ = Result{ok, ok ? "Starting two games: the host now, the guest a few seconds later." : error, ""};
    }
    ImGui::SameLine(0.0f, Px(14.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(9.0f));
    bool pad = lt.host_pad;
    if (Checkbox("Pads on the host too", &pad, !lt.running)) model_.set_local_test_host_pad(pad);
    Gap(2.0f);
    {
        // The guest game's my_character: the quickest way to see a chosen character (in the host window).
        const float row_y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(row_y + Px(8.0f));
        Text("Guest appears as", col::kTextDim, kSmall, nullptr, -1.0f);
        ImGui::SameLine(0.0f, Px(10.0f));
        ImGui::SetCursorPosY(row_y);
        std::string character = lt.guest_character;
        if (CharacterCombo("##guestcharacter", &character, 200.0f, !lt.running, model_.debug_options().allow_other_characters))
            model_.set_local_test_guest_character(character);
        ImGui::SameLine(0.0f, Px(12.0f));
        // Not row_y + 8 like the label before the combo: after SameLine ImGui already adds the
        // combo's frame padding (the line's text offset), so +8 would put this text 8 px too low.
        ImGui::SetCursorPosY(row_y);
        Text("in the host window. The host is always Sam.", col::kTextFaint, kSmall, nullptr, -1.0f);
        // Both local-test games get the Debug option's partner_body: with Hologram neither sends
        // joint frames, so the host window shows the guest as a hologram.
        if (!model_.debug_options().real_partner_body) Message(Icon::warning, col::kWarn, kHologramNote, kSmall, 0.0f, col::kTextDim);
        // The local test plays the online session (host_save=1): the waiting screen, shared vehicles, riding, hits.
        if (!model_.debug_options().host_save) Message(Icon::warning, col::kWarn, kHostSaveOffNote, kSmall, 0.0f, col::kTextDim);
    }
    if (lt.running || !lt.headline.empty()) {
        const ImU32 c = lt.tone == LinkView::Tone::good ? col::kGood : lt.tone == LinkView::Tone::bad ? col::kBad
                        : lt.tone == LinkView::Tone::warn ? col::kWarn : col::kCool;
        Message(lt.tone == LinkView::Tone::good ? Icon::check : Icon::signal, c, lt.headline, kBody);
        if (lt.running) {
            const std::string games = "Host game " + (lt.host_pid ? "pid " + std::to_string(lt.host_pid) : std::string("starting")) + " (" +
                                      (lt.host_state.empty() ? std::string("no status yet") : lt.host_state) + ")   Guest game " +
                                      (lt.guest_pid ? "pid " + std::to_string(lt.guest_pid) : std::string("starts soon")) + " (" +
                                      (lt.guest_state.empty() ? std::string("no status yet") : lt.guest_state) + ")";
            Text(games.c_str(), col::kTextDim, kSmall, GetFonts().mono, 0.0f);
        }
        Text(lt.detail.c_str(), col::kTextDim, kSmall, nullptr, 0.0f);
        if (!lt.features_off.empty()) Message(Icon::warning, col::kWarn, "Off: " + lt.features_off, kSmall, 0.0f, col::kText);
    }
    EndCard();
    ResultLine(debug_result_);
    (void)phase;
}

void LauncherUi::DebugOptionsTab(Phase phase) {
    const bool session = ui::SessionActive(phase);
    BeginCard("##debugstart");
    SectionLabel("START A TEST");
    const bool can = ui::CanStartSession(phase);
    if (Button("Debug tools only (solo)", ButtonKind::primary, ImVec2(232.0f, 40.0f), can, Icon::tools, true)) {
        CommitFields();
        std::string error;
        const bool ok = model_.debug_solo(error);
        debug_result_ = Result{ok, ok ? "Starting DEATH STRANDING with the debug tools only." : error, ""};
    }
    ImGui::SameLine(0.0f, Px(10.0f));
    if (Button("Echo test", ButtonKind::secondary, ImVec2(150.0f, 40.0f), can, Icon::echo, true)) {
        CommitFields();
        std::string error;
        const bool ok = model_.solo_test(error);
        debug_result_ = Result{ok, ok ? "Starting DEATH STRANDING with your echo." : error, ""};
    }
    Gap(2.0f);
    Text(session ? "A game is running: the options below apply to the next game you start."
                 : "Debug tools only: the in-game debug menu and the log, no partner, no network. Echo: a copy of you replays your moves.",
         col::kTextDim, kSmall, nullptr, 0.0f);
    ResultLine(debug_result_);
    EndCard();
    Gap(4.0f);

    // While a slider is dragged the value lives here; it is saved when released.
    DebugOptions o = debug_editing_ ? debug_edit_ : model_.debug_options();
    bool changed = false, committed = false;
    BeginCard("##debugopts");
    SectionLabel("OPTIONS");
    const float avail = ImGui::GetContentRegionAvail().x / theme::Scale();
    const float col_w = std::floor((avail - 20.0f) * 0.5f);
    float delay = static_cast<float>(o.echo_delay_s), offset = static_cast<float>(o.echo_offset_m);
    // Two columns; both groups of a row start at the same height.
    // (The right one is placed with SetCursorPos, not SameLine, which would move it to the slider's text baseline.)
    const float left_x = ImGui::GetCursorPosX(), right_x = left_x + Px(col_w + 20.0f);
    float row_y = ImGui::GetCursorPosY();
    changed |= SliderFloatField("##echodelay", "ECHO DELAY", &delay, 0.0f, 10.0f, "%.1f s", col_w);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetCursorPos(ImVec2(right_x, row_y));
    changed |= SliderIntField("##sendhz", "SEND RATE", &o.send_hz, 1, 120, "%d per second", col_w);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    row_y = ImGui::GetCursorPosY();
    changed |= SliderFloatField("##echooffset", "ECHO OFFSET (SIDEWAYS)", &offset, 0.0f, 20.0f, "%.1f m", col_w);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SetCursorPos(ImVec2(right_x, row_y));
    changed |= SliderIntField("##interp", "PARTNER INTERPOLATION DELAY", &o.interp_delay_ms, 0, 1000, "%d ms", col_w);
    committed |= ImGui::IsItemDeactivatedAfterEdit();
    o.echo_delay_s = delay;
    o.echo_offset_m = offset;
    Gap(4.0f);
    // session.ini partner_body: how YOUR partner is drawn on YOUR screen (the hologram is the fallback).
    // It also decides whether this game sends its character at all: the pose hook (and with it the
    // STJ1 joint frames that carry my_character) is installed only for partner_body=real.
    if (scroll_to_partner_body_ && page_window_)   // screenshot scene 40: the page scrolled to this section
        ImGui::SetScrollFromPosY(page_window_, ImGui::GetCursorScreenPos().y - page_window_->Pos.y - Px(12.0f), 0.0f);
    SectionLabel("PARTNER BODY");
    {
        static const char* const kBodies[2] = {"Real character (experimental)", "Hologram"};
        int body = o.real_partner_body ? 0 : 1;
        if (Segmented("##partnerbody", kBodies, 2, &body, std::fmin(col_w * 2.0f + 20.0f, 460.0f), true)) {
            o.real_partner_body = body == 0;
            committed = true;
        }
        Text(o.real_partner_body ? "Your partner shows as a solid character that copies their moves, and they see you as your character. "
                                   "If either game uses Hologram, or the character cannot be shown, the hologram is used."
                                 : "Your partner shows as a see-through hologram, and you appear to them as one too: your character "
                                   "(Join) is not sent.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini self_character: on your own screen Sam is replaced by your character too. Needs
        // Real character (the pose hook); written only for a guest whose character is not Sam.
        Gap(2.0f);
        committed |= Checkbox("See yourself as your character", &o.self_character, o.real_partner_body);
        Text(o.real_partner_body && o.self_character ? "On your own screen you appear as your character, Fragile unless you picked another (the host is always Sam)."
                                                     : "On your own screen you stay Sam.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini self_voice: auto (Sam's voice muted on your game when your character is not Sam) or sam.
        Gap(2.0f);
        committed |= Checkbox("Mute Sam's voice when you are not Sam", &o.mute_sam_voice, true);
        Text(o.mute_sam_voice ? "Playing Fragile (or anyone but Sam), you no longer hear Sam's lines, grunts and breathing on your game. "
                                "Footsteps, gear, BB and radio calls stay. The host is always Sam and hears him."
                              : "You hear Sam's voice on your game whatever character you play.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini voice_sync: each player hears the other's voice from their body.
        Gap(2.0f);
        committed |= Checkbox("Hear your partner's voice", &o.partner_voice_sync, true);
        Text(o.partner_voice_sync ? "Your partner's lines, grunts and breathing play from their body when they are near you, and yours "
                                    "from yours. Only Sam's voice exists: a partner who is not Sam is silent."
                                  : "You do not hear your partner's voice, and they do not hear yours.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini sound_sync: each player hears the other's footsteps, foley and gun from their body.
        Gap(2.0f);
        committed |= Checkbox("Hear your partner's footsteps and sounds", &o.partner_sound_sync, true);
        Text(o.partner_sound_sync ? "Your partner's footsteps, movement, cargo and weapon sounds play from their body when they are "
                                    "near you, and yours from yours (experimental)."
                                  : "You do not hear your partner's footsteps and sounds, and they do not hear yours.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini partner_volume: the partner's sounds on this game (not their voice).
        if (o.partner_sound_sync) {
            Gap(2.0f);
            changed |= SliderIntField("##partnervol", "PARTNER SOUND VOLUME", &o.partner_volume, 0, 100, "%d %%", col_w);
            committed |= ImGui::IsItemDeactivatedAfterEdit();
            Text(o.partner_volume == 0 ? "Your partner's footsteps and sounds are not played on your game."
                                       : "How loud your partner's footsteps and sounds are on your game (not their voice).",
                 col::kTextFaint, kSmall, nullptr, 0.0f);
        }

        // session.ini name_tags: the partner's name above their head; ours goes to them.
        Gap(2.0f);
        committed |= Checkbox("Show player names above heads", &o.name_tags, true);
        Text(o.name_tags ? "Your partner's name floats above them, and yours above you on their screen (your Steam name, or the one set under Advanced)."
                         : "No names are shown, and your name is not sent.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini partner_collision: the partner's real body blocks you.
        Gap(2.0f);
        committed |= Checkbox("Players block each other", &o.partner_collision, o.real_partner_body);
        Text(!o.real_partner_body ? "Needs Partner body = Real character: a hologram partner never blocks you."
             : o.partner_collision ? "You cannot walk through your partner's body; it stops you like another character. Off in vehicles, menus and cutscenes."
                                   : "You can walk through your partner.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // session.ini partner_vehicle_damage: the partner's vehicles hit you (docs/real-partner-body.md section 25).
        Gap(2.0f);
        committed |= Checkbox("Partner's vehicles can knock me down", &o.partner_vehicle_damage, true);
        Text(o.partner_vehicle_damage ? "Your partner's truck, trike or car knocks you down and hurts you like an enemy truck (never an instant kill), "
                                        "also when you jump off it at speed. Riding as the passenger, nothing hurts you. Your partner's own setting "
                                        "decides for them."
                                      : "Your partner's vehicles never hurt or knock you down.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
        // Launcher-only: which characters Join and the local test offer the guest.
        Gap(2.0f);
        committed |= Checkbox("Allow other characters", &o.allow_other_characters, true);
        Text(o.allow_other_characters ? "Join and the local test offer every character (experimental). The host is always Sam."
                                      : "Guests play Fragile in her porter suit. The host is always Sam.",
             col::kTextFaint, kSmall, nullptr, 0.0f);
    }
    Gap(4.0f);
    // session.ini host_save (Host and Join): the host-held save (docs/coop-session.md). Off = the old mode.
    if (scroll_to_coop_save_ && page_window_)   // screenshot scene 41: the page scrolled to this section
        ImGui::SetScrollFromPosY(page_window_, ImGui::GetCursorScreenPos().y - page_window_->Pos.y - Px(12.0f), 0.0f);
    SectionLabel("CO-OP SAVE");
    committed |= Checkbox("Host holds the save", &o.host_save, true);
    Text(o.host_save ? "The host's save is the one save of the session. Your partner's game loads a copy of it and waits until the host "
                       "starts; a guest's own saves are never opened."
                     : "Old mode: each player plays on their own save, and the two worlds are not the same.",
         col::kTextFaint, kSmall, nullptr, 0.0f);
    Gap(2.0f);
    committed |= Checkbox("Back up my saves before co-op", &o.backup_host_saves, o.host_save);
    Text(o.host_save && o.backup_host_saves ? "Before each game you host, a copy of your save folder goes to stranded_together\\backups "
                                              "(the newest 5 are kept)."
                                            : "No backup is made when you host.",
         col::kTextFaint, kSmall, nullptr, 0.0f);
    Gap(4.0f);
    committed |= Checkbox("In-game debug menu", &o.debug_menu, true);
    ImGui::SameLine(0.0f, Px(10.0f));
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
        const Font f(GetFonts().mono, kSmall + 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, Px(10.0f, 5.0f));
        ImGui::SetNextItemWidth(Px(76.0f));
        int key = o.debug_menu_key - 1;
        if (!o.debug_menu) ImGui::BeginDisabled();
        if (ImGui::Combo("##menukey", &key, ui::FunctionKeyItems())) {
            o.debug_menu_key = key + 1;
            committed = true;
        }
        if (!o.debug_menu) ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }
    ImGui::SameLine(0.0f, Px(8.0f));
    Text("opens it", col::kTextFaint, kSmall, nullptr, -1.0f);
    Gap(2.0f);
    committed |= Checkbox("Dev tools", &o.dev_tools, o.debug_menu);
    Text("A Dev tools page in the debug menu: spawn vehicles, change the weather, bring back enemies, refill, god mode, "
         "teleport to places. Spawning and world changes work only on a test save (the Local test).",
         col::kTextFaint, kSmall, nullptr, 0.0f);
    Gap(2.0f);
    committed |= Checkbox("Lock the mouse to the focused game window (Local test)", &o.mouse_lock, true);
    Text(o.mouse_lock ? "The cursor stays inside the game window you play in. F10 switches to the other game's window "
                        "(F11 when the menu key is F10); Alt+Tab leaves."
                      : "The cursor can leave the game window (the game holds it only in gameplay).",
         col::kTextFaint, kSmall, nullptr, 0.0f);
    Gap(2.0f);
    const float row_right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    committed |= Checkbox("Wake the partner clone", &o.partner_activate, true);
    ImGui::SameLine(0.0f, Px(28.0f));
    committed |= Checkbox("Verbose mod log", &o.verbose, true);
    // Reset sits at the right end of the checkbox row.
    ImGui::SameLine(row_right - Px(160.0f));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(7.0f));
    if (Button("Reset to defaults##debug", ButtonKind::quiet, ImVec2(160.0f, 32.0f), true, Icon::sliders, true)) {
        o = DebugOptions{};
        committed = true;
    }
    EndCard();
    // Sliders save when released; checkboxes, the key and Reset at once.
    if (committed) {
        model_.set_debug_options(o);
        debug_editing_ = false;
    } else if (changed) {
        debug_edit_ = o;
        debug_editing_ = true;
    }
}

void LauncherUi::DebugFilesTab(Phase phase) {
    RefreshLog(false);
    const InstallChecks c = model_.checks();
    const bool game = phase == Phase::launching || phase == Phase::in_game;
    const float avail = ImGui::GetContentRegionAvail().x / theme::Scale();
    const float gap = 8.0f;
    const float w3 = std::floor((avail - 2.0f * gap) / 3.0f);
    if (Button("Open mod folder", ButtonKind::secondary, ImVec2(w3, 38.0f), true, Icon::folder, true))
        if (shell_.open_folder) shell_.open_folder(model_.mod_dir());
    ImGui::SameLine(0.0f, Px(gap));
    if (Button("Open log##debuglog", ButtonKind::secondary, ImVec2(w3, 38.0f), true, Icon::logs, true))
        if (shell_.open_file) shell_.open_file(model_.mod_log_path());
    ImGui::SameLine(0.0f, Px(gap));
    if (Button("Copy error.txt", ButtonKind::secondary, ImVec2(w3, 38.0f), model_.has_crash_report(), Icon::copy, true)) {
        std::string text;
        if (model_.crash_report(text)) {
            ImGui::SetClipboardText(text.c_str());
            debug_result_ = Result{true, "The game's crash report (error.txt, " + std::to_string(CountLines(text)) + " lines) is on your clipboard.", ""};
        } else {
            debug_result_ = Result{false, "error.txt could not be read.", ""};
        }
    }
    ModSwitchButton(c, !ui::SessionActive(phase), w3);
    ImGui::SameLine(0.0f, Px(gap));
    if (Button("Clear session", ButtonKind::secondary, ImVec2(w3, 38.0f), !game && model_.session_pending(), Icon::trash, true)) {
        std::string error;
        const bool ok = model_.clear_session(error);
        debug_result_ = Result{ok, ok ? "session.ini is gone: the game starts unmodded until you start co-op or a test again." : error, ""};
    }
    ImGui::SameLine(0.0f, Px(gap));
    if (Button("Copy log##copylog", ButtonKind::quiet, ImVec2(w3, 38.0f), !log_text_.empty(), Icon::copy, true)) {
        const std::string all = model_.mod_log_tail(4u << 20);
        ImGui::SetClipboardText(all.c_str());
        debug_result_ = Result{true, "The mod log (" + std::to_string(CountLines(all)) + " lines) is on your clipboard.", ""};
    }
    if (!model_.has_crash_report())
        Message(Icon::info, col::kTextFaint, "No error.txt next to ds.exe: the game has not reported a crash.", kSmall);
    if (game) Message(Icon::info, col::kTextFaint, "A game is running: switching the mod off and Clear session wait until it closes.", kSmall);
    ResultLine(debug_result_);
    Gap(4.0f);

    SectionLabel("MOD LOG (LIVE)");
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(2.0f));
    Checkbox("Follow##follow", &log_follow_, true);
    const float h = ImGui::GetContentRegionAvail().y - Px(4.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::kField);
    ImGui::PushStyleColor(ImGuiCol_Border, col::kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Px(8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, Px(10.0f, 8.0f));
    ImGui::BeginChild("##logview", ImVec2(0.0f, std::fmax(h, Px(80.0f))), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    {
        const Font f(GetFonts().mono, kLabel);
        if (log_text_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, col::kTextFaint);
            ImGui::TextUnformatted("stranded_together.log is empty or does not exist yet: the mod writes it once the game runs with it.");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, col::kTextDim);
            ImGui::TextUnformatted(log_text_.c_str(), log_text_.c_str() + log_text_.size());
            ImGui::PopStyleColor();
        }
        if (log_scroll_end_) {
            ImGui::SetScrollHereY(1.0f);
            log_scroll_end_ = false;
        }
    }
    ImGui::EndChild();
}

// ---- Updates ---------------------------------------------------------------
void LauncherUi::UpdateCard(Phase phase) {
    const UpdateInfo u = model_.update_info();
    const ui::UpdateBanner banner = ui::UpdateBannerFor(u);
    if (banner == ui::UpdateBanner::none) return;
    if (ui::UpdateInProgress(u)) animating_ = true;
    const bool failed = banner == ui::UpdateBanner::failed;
    const bool updated = banner == ui::UpdateBanner::updated;
    const bool offer = banner == ui::UpdateBanner::available || failed;
    const ImU32 tone = updated ? col::kGood : failed ? col::kWarn : col::kAccent;
    const ImU32 soft = updated ? col::kGoodSoft : failed ? col::kWarnSoft : col::kAccentSoft;
    BeginCard("##update", 0.0f, col::kCard, updated ? col::kGoodBorder : failed ? col::kWarnBorder : col::kAccentBorder);
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorPos();
    const bool notes_button = offer && !u.html_url.empty();
    const float update_w = 138.0f, notes_w = 124.0f;
    const float buttons_w = offer ? Px(update_w) + (notes_button ? Px(notes_w + 8.0f) : 0.0f) : 0.0f;
    IconBadge(updated ? Icon::check : failed ? Icon::warning : Icon::download, tone, soft, 32.0f);
    ImGui::SameLine(0.0f, Px(12.0f));
    ImGui::BeginGroup();
    const float text_w = std::fmax(Px(160.0f), avail - Px(44.0f) - (offer ? buttons_w + Px(16.0f) : 0.0f));
    std::string title;
    switch (banner) {
    case ui::UpdateBanner::available: title = "Update available: version " + u.latest_version; break;
    case ui::UpdateBanner::downloading: title = "Downloading version " + u.latest_version; break;
    case ui::UpdateBanner::installing: title = "Installing version " + u.latest_version; break;
    case ui::UpdateBanner::failed: title = "Version " + u.latest_version + " was not installed"; break;
    case ui::UpdateBanner::updated: title = "Updated to version " + u.updated_to; break;
    case ui::UpdateBanner::none: break;
    }
    Text(title.c_str(), col::kText, kBody + 1.0f, GetFonts().semibold, -1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - Px(4.0f));
    switch (banner) {
    case ui::UpdateBanner::available: {
        std::string notes = ui::NotesExcerpt(u.release_notes, 2, 150);
        if (notes.empty()) notes = "Both players need the same version.";
        Text(notes.c_str(), col::kTextDim, kSmall, nullptr, text_w);
        break;
    }
    case ui::UpdateBanner::downloading: {
        ProgressBar(u.progress, text_w / Scale(), 6.0f, col::kAccent);
        const std::string bytes = u.download_size ? ui::FormatBytes(u.bytes_downloaded) + " of " + ui::FormatBytes(u.download_size)
                                                  : ui::FormatBytes(u.bytes_downloaded);
        Text(bytes.c_str(), col::kTextDim, kSmall, nullptr, text_w);
        break;
    }
    case ui::UpdateBanner::installing: {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Spinner(ImVec2(p.x + Px(7.0f), p.y + Px(9.0f)), 6.0f, col::kAccent, time_);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(20.0f));
        Text("Checking the files, replacing the launcher and the mod, then restarting...", col::kTextDim, kSmall, nullptr, text_w - Px(20.0f));
        break;
    }
    case ui::UpdateBanner::failed: Text(u.error.c_str(), col::kTextDim, kSmall, nullptr, text_w); break;
    case ui::UpdateBanner::updated:
        Text("Both players need the same version: ask your partner to update too.", col::kTextDim, kSmall, nullptr, text_w);
        break;
    case ui::UpdateBanner::none: break;
    }
    ImGui::EndGroup();
    if (offer) {
        const ImVec2 after = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(start.x + avail - buttons_w, start.y));
        if (notes_button) {
            if (Button("Release notes##update", ButtonKind::quiet, ImVec2(notes_w, 34.0f)))
                if (shell_.open_url) shell_.open_url(u.html_url);
            ImGui::SameLine(0.0f, Px(8.0f));
        }
        if (Button(failed ? "Try again##update" : "Update now##update", ButtonKind::primary, ImVec2(update_w, 34.0f), ui::CanStartUpdate(phase, u),
                   Icon::download, true))
            model_.start_update();
        ImGui::SetCursorPos(ImVec2(start.x, std::fmax(after.y, start.y + Px(40.0f))));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }
    if (offer && !ui::CanStartUpdate(phase, u) && ui::SessionActive(phase))
        Message(Icon::info, col::kTextFaint, "You can update once the game has closed.", kSmall);
    EndCard();
    Gap(2.0f);
}

void LauncherUi::UpdatesSection(Phase phase) {
    const UpdateInfo u = model_.update_info();
    if (u.status == UpdateStatus::checking || ui::UpdateInProgress(u)) animating_ = true;
    BeginCard("##updates");
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 start = ImGui::GetCursorPos();
    const float button_w = 176.0f;
    SectionLabel("UPDATES");
    const bool error = u.status == UpdateStatus::error;
    const bool available = u.status == UpdateStatus::available;
    const bool latest = u.status == UpdateStatus::up_to_date;
    const ImU32 color = error ? col::kWarn : available ? col::kAccent : latest ? col::kGood : col::kTextDim;
    const Icon icon = error ? Icon::warning : available ? Icon::download : latest ? Icon::check : Icon::info;
    const std::string status = ui::UpdateStatusText(u);
    if (u.status == UpdateStatus::checking) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Spinner(ImVec2(p.x + Px(7.0f), p.y + Px(9.0f)), 6.0f, col::kCool, time_);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Px(20.0f));
        Text(status.c_str(), col::kTextDim, kSmall, nullptr, avail - Px(button_w + 36.0f));
    } else {
        Message(icon, color, status, kSmall, avail - Px(button_w + 16.0f), col::kText);
    }
    bool auto_check = u.auto_check;
    if (Checkbox("Check for updates when the launcher starts", &auto_check, true)) model_.set_auto_update_check(auto_check);
    const ImVec2 after = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x + avail - Px(button_w), start.y + Px(6.0f)));
    if (ui::CanStartUpdate(phase, u) || ui::UpdateInProgress(u)) {
        if (Button("Update now##about", ButtonKind::primary, ImVec2(button_w, 36.0f), ui::CanStartUpdate(phase, u), Icon::download, true))
            model_.start_update();
    } else if (Button("Check for updates##about", ButtonKind::secondary, ImVec2(button_w, 36.0f), ui::CanCheckForUpdates(u))) {
        model_.check_for_updates();
    }
    ImGui::SetCursorPos(ImVec2(start.x, std::fmax(after.y, start.y + Px(44.0f))));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    EndCard();
}

// ---- About -----------------------------------------------------------------
void LauncherUi::AboutPage(Phase phase) {
    if (logo_.id) {
        const float lw = Px(170.0f);
        const float lh = lw * logo_.height / logo_.width;
        const float x = ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - lw) * 0.5f;
        ImGui::SetCursorPosX(x);
        ImGui::Image(ImTextureRef(logo_.id), ImVec2(lw, lh));
    }
    {
        ImDrawList* d = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float center = p.x + ImGui::GetContentRegionAvail().x * 0.5f;
        SpacedTitle(d, center, p.y + Px(4.0f), "STRANDED  TOGETHER", 7.0f, 3u, 20.0f);
        ImGui::Dummy(ImVec2(0.0f, Px(50.0f)));
    }
    {
        const std::string version = "Version " + model_.version();
        const Font f(GetFonts().regular, kSmall);
        const float tw = ImGui::CalcTextSize(version.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - tw) * 0.5f);
        Text(version.c_str(), col::kTextFaint, kSmall, nullptr, -1.0f);
    }
    Gap(4.0f);
    UpdatesSection(phase);
    Gap(2.0f);
    BeginCard("##aboutcard");
    Text("Two-player online co-op for DEATH STRANDING DIRECTOR'S CUT: two porters in one world, connected over Steam or your own network.",
         col::kText, kBody, nullptr, 0.0f);
    Gap(2.0f);
    Text("A fan project. Not affiliated with, endorsed or sponsored by Kojima Productions, 505 Games or Sony Interactive Entertainment. "
         "DEATH STRANDING is a trademark of its owners.",
         col::kTextDim, kSmall, nullptr, 0.0f);
    EndCard();
    Gap(4.0f);
    SectionLabel("BUILT WITH");
    static const char* const kCredits[][2] = {
        {"Diamond Dogs Together", "the launcher and network code this is built on (same author)"},
        {"Dear ImGui", "Omar Cornut and contributors, MIT"},
        {"MinHook", "Tsuda Kageyu, BSD 2-clause"},
    };
    for (const auto& credit : kCredits) {
        Text(credit[0], col::kText, kSmall, GetFonts().semibold, -1.0f);
        ImGui::SameLine(ImGui::GetCursorPosX() + Px(170.0f));
        Text(credit[1], col::kTextDim, kSmall, nullptr, -1.0f);
    }
}

// ---- Screenshot scenes -----------------------------------------------------
int LauncherUi::SceneCount() { return ui::SceneCount(); }
const char* LauncherUi::SceneName(int index) { return ui::SceneName(static_cast<ui::Scene>(index)); }

void LauncherUi::PrepareScene(int index, MockModel& mock) {
    if (ImGui::GetCurrentContext() && ImGui::GetCurrentContext()->OpenPopupStack.Size > 0) ImGui::ClosePopupToLevel(0, false);
    play_tab_ = ui::PlayTab::host;
    code_buf_[0] = '\0';
    checked_text_.assign("\x01");
    hosted_code_.clear();
    play_error_.clear();
    copied_until_ = -1.0;
    advanced_open_ = false;
    logs_result_ = tool_result_ = debug_result_ = Result{};
    debug_tab_ = ui::DebugTab::options;
    log_text_.clear();
    log_refreshed_at_ = -1e9;
    log_follow_ = true;
    log_scroll_end_ = false;
    mock.use_manual_clock(1000.0);
    mock.set_debug_options(DebugOptions{});
    mock.set_crash_report(false);
    mock.set_scenario(MockScenario::ready);
    mock.set_transport(Transport::steam);
    LanSettings lan;
    lan.peer_ip = "192.168.1.20";
    mock.set_lan(lan);
    mock.set_player_name("");
    {
        // The stored picks back to the default (they can only be set with other characters allowed).
        DebugOptions all;
        all.allow_other_characters = true;
        mock.set_debug_options(all);
        mock.set_my_character(core::kGuestCharacter);
        mock.set_local_test_guest_character(core::kGuestCharacter);
        mock.set_debug_options(DebugOptions{});
    }
    open_character_list_ = false;
    scroll_to_partner_body_ = false;
    scroll_to_coop_save_ = false;
    mock.set_test_save_view(TestSaveView{});
    mock.jump_to_local_test(-1.0);
    {
        UpdateInfo none;
        none.current_version = mock.version();
        mock.set_update_info(none);
    }
    auto copy_into = [&](const std::string& s) { strncpy_s(code_buf_, sizeof(code_buf_), s.c_str(), _TRUNCATE); };
    Page page = Page::play;
    switch (static_cast<ui::Scene>(index)) {
    case ui::Scene::play_host: break;
    case ui::Scene::play_host_launching:
        mock.jump_to_session(Role::host, Transport::steam, Phase::launching, 1.0);
        hosted_code_ = mock.join_code();
        copied_until_ = 1e9;
        break;
    case ui::Scene::play_join: play_tab_ = ui::PlayTab::join; break;
    case ui::Scene::play_join_invalid: {
        play_tab_ = ui::PlayTab::join;
        std::string bad = MockModel::PartnerCode();
        const std::size_t at = bad.find('@');
        bad[at - 1] = bad[at - 1] == 'A' ? 'B' : 'A';
        copy_into(bad);
        break;
    }
    case ui::Scene::play_join_valid:   // the guest's fixed character: Fragile (porter suit)
        play_tab_ = ui::PlayTab::join;
        copy_into(MockModel::PartnerCode());
        break;
    case ui::Scene::play_join_characters: {   // the character list open (Debug option "Allow other characters")
        play_tab_ = ui::PlayTab::join;
        copy_into(MockModel::PartnerCode());
        DebugOptions o;
        o.allow_other_characters = true;
        mock.set_debug_options(o);
        mock.set_my_character("fragile");
        open_character_list_ = true;
        break;
    }
    case ui::Scene::debug_options_characters: {   // the Debug option that brings the other characters back, on
        DebugOptions o;
        o.allow_other_characters = true;
        mock.set_debug_options(o);
        scroll_to_partner_body_ = true;
        page = Page::debug;
        break;
    }
    case ui::Scene::debug_options_coop: {   // the host-held save and the dev tools, both on
        DebugOptions o;
        o.host_save = true;
        o.dev_tools = true;
        mock.set_debug_options(o);
        scroll_to_coop_save_ = true;
        page = Page::debug;
        break;
    }
    case ui::Scene::debug_local_test_off: {   // "Host holds the save" off: the local test card says what is off
        DebugOptions o;
        o.host_save = false;
        mock.set_debug_options(o);
        TestSaveView t = mock.test_save();
        t.folder = "D:\\Downloads\\DeathStranding-100-percent-save";
        t.folder_ok = true;
        t.folder_note = "23 save slots found";
        t.seeded = true;
        mock.set_test_save_view(t);
        mock.jump_to_local_test(-1.0);
        debug_tab_ = ui::DebugTab::saves;
        page = Page::debug;
        break;
    }
    case ui::Scene::play_join_hologram: {   // Hologram sends no joint frames: the character is not shown
        play_tab_ = ui::PlayTab::join;
        copy_into(MockModel::PartnerCode());
        DebugOptions o;
        o.real_partner_body = false;
        mock.set_debug_options(o);
        break;
    }
    case ui::Scene::play_solo: play_tab_ = ui::PlayTab::solo; break;
    case ui::Scene::play_lan:
        play_tab_ = ui::PlayTab::join;
        copy_into(MockModel::PartnerCode());
        mock.set_transport(Transport::udp);
        mock.set_player_name("Sam");
        advanced_open_ = true;
        break;
    case ui::Scene::play_untested_version: mock.set_scenario(MockScenario::untested_version); break;
    case ui::Scene::play_steam_offline: mock.set_scenario(MockScenario::steam_offline); break;
    case ui::Scene::play_dev_build: mock.set_scenario(MockScenario::dev_build); break;
    case ui::Scene::play_mod_mismatch: mock.set_scenario(MockScenario::mod_mismatch); break;
    case ui::Scene::not_installed: mock.set_scenario(MockScenario::not_installed); break;
    case ui::Scene::mod_foreign: mock.set_scenario(MockScenario::mod_foreign); break;
    case ui::Scene::mod_disabled: mock.set_scenario(MockScenario::mod_disabled); break;
    case ui::Scene::wrong_folder: mock.set_scenario(MockScenario::wrong_folder); break;
    case ui::Scene::ingame_launching:
        mock.jump_to_session(Role::join, Transport::steam, Phase::launching, 1.5);
        page = Page::in_game;
        break;
    case ui::Scene::ingame_host_waiting:
        mock.jump_to_session(Role::host, Transport::steam, Phase::in_game, 2.0);
        hosted_code_ = mock.join_code();
        page = Page::in_game;
        break;
    case ui::Scene::ingame_connected:
        mock.jump_to_session(Role::join, Transport::steam, Phase::in_game, 12.0);
        page = Page::in_game;
        break;
    case ui::Scene::ingame_echo:
        mock.jump_to_session(Role::echo, Transport::udp, Phase::in_game, 12.0);
        page = Page::in_game;
        break;
    case ui::Scene::ingame_solo:
        mock.jump_to_session(Role::solo, Transport::udp, Phase::in_game, 12.0);
        page = Page::in_game;
        break;
    case ui::Scene::ingame_not_responding:
        mock.jump_to_session(Role::join, Transport::steam, Phase::in_game, 12.0);
        mock.set_link_override(MockModel::LinkOverride::not_responding);
        page = Page::in_game;
        break;
    case ui::Scene::ingame_lost:
        mock.jump_to_session(Role::host, Transport::udp, Phase::in_game, 40.0);
        mock.set_link_override(MockModel::LinkOverride::lost);
        hosted_code_ = mock.join_code();
        page = Page::in_game;
        break;
    case ui::Scene::ingame_after_game:
        mock.jump_to_session(Role::join, Transport::steam, Phase::after_game, 1.0);
        page = Page::in_game;
        break;
    case ui::Scene::tools: page = Page::tools; break;
    case ui::Scene::tools_result: {
        page = Page::tools;
        std::string path, error;
        const bool ok = mock.collect_logs(path, error);
        tool_result_ = Result{ok, ok ? "Saved to your Desktop: " + FileName(path) + "." : error, path};
        break;
    }
    case ui::Scene::tools_in_game:
        mock.jump_to_session(Role::host, Transport::steam, Phase::in_game, 12.0);
        hosted_code_ = mock.join_code();
        page = Page::tools;
        break;
    case ui::Scene::debug_options: {
        DebugOptions o;
        o.echo_delay_s = 3.5;
        o.verbose = true;
        mock.set_debug_options(o);
        page = Page::debug;
        break;
    }
    case ui::Scene::debug_files:
        mock.set_crash_report(true);
        debug_tab_ = ui::DebugTab::files;
        page = Page::debug;
        break;
    case ui::Scene::debug_in_game:
        mock.jump_to_session(Role::solo, Transport::udp, Phase::in_game, 12.0);
        debug_tab_ = ui::DebugTab::files;
        page = Page::debug;
        break;
    case ui::Scene::about: page = Page::about; break;
    case ui::Scene::debug_test_saves: {
        TestSaveView t = mock.test_save();
        t.folder = "D:\\Downloads\\DeathStranding-100-percent-save";
        t.folder_ok = true;
        t.folder_note = "23 save slots found";
        t.seeded = true;
        t.result = "Your own saves were not changed (every file checked by SHA-256 before and after).";
        t.tone = TestSaveView::Tone::good;
        mock.set_test_save_view(t);
        mock.jump_to_local_test(-1.0);
        debug_tab_ = ui::DebugTab::saves;
        page = Page::debug;
        break;
    }
    case ui::Scene::debug_local_test: {
        TestSaveView t = mock.test_save();
        t.folder = "D:\\Downloads\\DeathStranding-100-percent-save";
        t.folder_ok = true;
        t.folder_note = "23 save slots found";
        t.seeded = true;
        mock.set_test_save_view(t);
        mock.jump_to_local_test(30.0);
        debug_tab_ = ui::DebugTab::saves;
        page = Page::debug;
        break;
    }
    case ui::Scene::play_update_available: mock.set_update_info(MockModel::MockRelease()); break;
    case ui::Scene::play_update_downloading: {
        UpdateInfo u = MockModel::MockRelease();
        u.status = UpdateStatus::downloading;
        u.progress = 0.45f;
        u.bytes_downloaded = static_cast<std::uint64_t>(0.45 * static_cast<double>(u.download_size));
        mock.set_update_info(u);
        break;
    }
    case ui::Scene::play_update_failed: {
        UpdateInfo u = MockModel::MockRelease();
        u.status = UpdateStatus::error;
        u.error = "Quit DEATH STRANDING first: the update replaces files the game uses. Then press Update now again.";
        mock.set_update_info(u);
        break;
    }
    case ui::Scene::play_updated: {
        UpdateInfo u;
        u.status = UpdateStatus::up_to_date;
        u.current_version = "0.2.0";
        u.latest_version = "0.2.0";
        u.updated_to = "0.2.0";
        mock.set_update_info(u);
        break;
    }
    case ui::Scene::about_updates: {
        UpdateInfo u;
        u.status = UpdateStatus::up_to_date;
        u.current_version = mock.version();
        u.latest_version = "0.1.0";
        u.auto_check = false;
        mock.set_update_info(u);
        page = Page::about;
        break;
    }
    case ui::Scene::count: break;
    }
    LoadFields();
    const Phase phase = mock.phase();
    last_phase_ = phase;
    first_frame_ = false;
    page_ = ui::PageEnabled(phase, page) ? page : ui::DefaultPage(phase);
}

}  // namespace st
