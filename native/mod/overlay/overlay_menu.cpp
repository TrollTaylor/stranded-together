// Debug menu layout. Render thread only; all state here is the menu's own.
#include "overlay_menu.hpp"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imgui.h"
#include "log.hpp"
#include "overlay_keys.hpp"
#include "overlay_positions.hpp"

namespace st::overlay::menu {
namespace {

const ImVec4 kGreen(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kAmber(0.98f, 0.75f, 0.30f, 1.0f);
const ImVec4 kRed(0.95f, 0.42f, 0.40f, 1.0f);
const ImVec4 kDim(0.62f, 0.66f, 0.72f, 1.0f);

// ---------------------------------------------------------------- state
std::wstring g_positions_path;
std::vector<SavedPosition> g_positions;
int g_selected = -1;
char g_new_name[64] = {};
char g_positions_note[160] = {};

double g_target[3] = {};
char g_paste[96] = {};

bool g_echo_init = false;
float g_echo_delay = 2.0f, g_echo_offset = 0.0f;

bool g_tod_init = false;
float g_tod_hours = 12.0f;

std::vector<LogLine> g_lines;
std::vector<LogLine> g_fetch;
std::uint64_t g_last_seq = 0;
char g_filter[128] = {};
bool g_autoscroll = true;

int g_select_tab = -1;

std::uint32_t g_serials[static_cast<std::size_t>(debug::Action::count)] = {};

// ---------------------------------------------------------------- helpers
void Text(const ImVec4* color, const char* fmt, ...) IM_FMTARGS(2);
void Text(const ImVec4* color, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (color) ImGui::TextColored(*color, "%s", buf);
    else ImGui::TextUnformatted(buf);
}

// Two-column property table row.
void Prop(const char* label, const char* fmt, ...) IM_FMTARGS(2);
void Prop(const char* label, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(kDim, "%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(buf);
}

bool BeginProps(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) return false;
    ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void Tooltip(const char* text) {
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

const char* FormatName(int f) {
    switch (f) {
        case 28: return "R8G8B8A8_UNORM";
        case 29: return "R8G8B8A8_UNORM_SRGB";
        case 87: return "B8G8R8A8_UNORM";
        case 91: return "B8G8R8A8_UNORM_SRGB";
        case 24: return "R10G10B10A2_UNORM (HDR10)";
        case 10: return "R16G16B16A16_FLOAT (scRGB)";
        case 0: return "-";
        default: return "other";
    }
}

double Dist(const double (&a)[3], const double (&b)[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Result chip for the newest request of an action.
void Chip(debug::Action a) {
    const std::uint32_t serial = g_serials[static_cast<std::size_t>(a)];
    if (!serial) return;
    debug::Result r;
    ImGui::SameLine();
    if (debug::ResultFor(serial, &r)) {
        ImGui::TextColored(r.ok ? kGreen : kRed, "%s", r.text);
    } else {
        ImGui::TextColored(kAmber, "queued...");
    }
}

void Send(const debug::Request& req) {
    const std::uint32_t serial = debug::Queue(req);
    g_serials[static_cast<std::size_t>(req.action)] = serial;
    if (!serial) Log("[debug] menu: action queue full, '%s' dropped", debug::ActionName(req.action));
}

void Send(debug::Action a) {
    debug::Request r;
    r.action = a;
    Send(r);
}

// A registry button with tooltip and result chip.
void ActionButton(const debug::ActionInfo& a) {
    ImGui::PushID(static_cast<int>(a.id));
    if (ImGui::Button(a.label)) Send(a.id);
    Tooltip(a.help);
    Chip(a.id);
    ImGui::PopID();
}

// Every button row of a tab, in registry order.
void ActionButtons(debug::Tab tab) {
    std::size_t n = 0;
    const debug::ActionInfo* all = debug::Actions(&n);
    for (std::size_t i = 0; i < n; ++i)
        if (all[i].tab == tab && all[i].button) ActionButton(all[i]);
}

void SavePositionsFile() {
    if (g_positions_path.empty()) return;
    if (SavePositions(g_positions_path, g_positions))
        std::snprintf(g_positions_note, sizeof(g_positions_note), "saved %zu position(s)", g_positions.size());
    else
        std::snprintf(g_positions_note, sizeof(g_positions_note), "could not write positions.txt (error %lu)", GetLastError());
}

void TeleportTo(double x, double y, double z, game::TeleportMode mode = game::TeleportMode::place) {
    debug::Request r;
    r.action = debug::Action::teleport;
    r.v[0] = x, r.v[1] = y, r.v[2] = z;
    r.i = static_cast<std::int32_t>(mode);
    Send(r);
}

// The game-side readout (game_actions.cpp), Status tab.
void GameSection(const debug::Snapshot& s) {
    const game::Diagnostics& d = s.diag;
    ImGui::SeparatorText("Game");
    if (!d.resolved) {
        Text(&kDim, "No game data yet.");
        return;
    }
    char a[192], b[192];
    if (BeginProps("game")) {
        if (d.state_valid) {
            game::DescribeGameStateFlags(d.state_flags, a, sizeof(a));
            const bool stop = ((d.state_flags | d.state_requested) & game::kStoppingGameEventMask) != 0;
            Prop("game state", "0x%llX  %s%s", static_cast<unsigned long long>(d.state_flags), a, stop ? "  (stopping event)" : "");
            if (d.state_requested) {
                game::DescribeGameStateFlags(d.state_requested, b, sizeof(b));
                Prop("requested", "0x%llX  %s", static_cast<unsigned long long>(d.state_requested), b);
            }
            Prop("load phase", "%d%s", d.state_phase, d.state_phase == 2 ? " (setup after a load)" : "");
        } else {
            Prop("game state", "n/a");
        }
        if (d.module_valid) {
            game::DescribeLoadMask(d.load_mask, a, sizeof(a));
            Prop("load mask", "0x%X  %s%s", d.load_mask, a, d.load_reason ? "  (load reason set)" : "");
        } else {
            Prop("load mask", "n/a");
        }
        if (d.area_valid) {
            const char* an = game::AreaName(d.area);
            Prop("area", "%u  %s", d.area, an ? an : "");
        } else {
            Prop("area", "n/a (read only during an area change)");
        }
        if (d.tod_valid) {
            game::FormatClock(d.time_of_day, a, sizeof(a));
            Prop("time of day", "%s  day %d, cycle %s", a, d.day, d.day_night_cycle ? "on" : "FROZEN");
        } else {
            Prop("time of day", "n/a");
        }
        if (d.health_valid) Prop("health", "%.1f / %.1f", static_cast<double>(d.health), static_cast<double>(d.health_max));
        else Prop("health", "n/a");
        if (d.player_system_valid && d.pushed_transform) Prop("player system", "pushed transform pending");
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(kDim, "teleport");
        ImGui::TableSetColumnIndex(1);
        if (d.teleport_ready) ImGui::TextColored(kGreen, "ready");
        else ImGui::TextColored(kAmber, "blocked: %s", d.teleport_block);
        if (d.last_teleport[0]) Prop("last teleport", "%s", d.last_teleport);
        ImGui::EndTable();
    }
    if (d.disabled[0]) {
        ImGui::PushTextWrapPos(0.0f);
        Text(&kRed, "Disabled: %s", d.disabled);
        ImGui::PopTextWrapPos();
    }
}

// ---------------------------------------------------------------- tabs
void Header(const Context& c) {
    const debug::Snapshot& s = *c.snap;
    Text(&kDim, "role");
    ImGui::SameLine();
    ImGui::TextUnformatted(s.role);
    ImGui::SameLine(0, 18);
    Text(&kDim, "link");
    ImGui::SameLine();
    const bool connected = std::strcmp(s.link_state, "connected") == 0;
    ImGui::TextColored(!s.net ? kDim : connected ? kGreen : kAmber, "%s", s.link_state);
    if (s.net && connected) {
        ImGui::SameLine();
        Text(nullptr, "%u ms", s.rtt_ms);
    }
    ImGui::SameLine(0, 18);
    Text(&kDim, "fps");
    ImGui::SameLine();
    Text(nullptr, "%.0f", c.stats->fps);
    ImGui::SameLine(0, 18);
    Text(&kDim, "tick");
    ImGui::SameLine();
    Text(nullptr, "%.0f Hz", s.tick_hz);
    if (s.serial == 0) Text(&kAmber, "No data from the game yet (%s).", s.mod_state);
}

void StatusTab(const Context& c) {
    const debug::Snapshot& s = *c.snap;
    if (BeginProps("status")) {
        Prop("build", "%s", c.build);
        Prop("role", "%s", s.role);
        Prop("mod", "%s", s.mod_state);
        if (s.net) {
            Prop("link", "%s  %s %s", s.link_state, s.transport, s.route);
            Prop("peer", "%s", s.peer[0] ? s.peer : "-");
            Prop("rtt", "%u ms", s.rtt_ms);
            Prop("frames", "rx %llu  tx %llu", static_cast<unsigned long long>(s.rx), static_cast<unsigned long long>(s.tx));
            if (s.reason[0]) Prop("reason", "%s", s.reason);
        } else {
            Prop("link", "%s", s.link_state);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Sam (local player)");
    if (BeginProps("local")) {
        Prop("entity", "%p%s", reinterpret_cast<void*>(s.local_entity), s.local_present ? "" : "  (none: title / loading)");
        Prop("state", "%s%s", s.local_valid ? "transform ok" : "no transform", s.local_present ? (s.local_awake ? ", awake" : ", asleep") : "");
        Prop("flags", "0x%llX", static_cast<unsigned long long>(s.local_flags));
        if (s.local_valid) Prop("position", "%.2f  %.2f  %.2f", s.local_pos[0], s.local_pos[1], s.local_pos[2]);
        Prop("speed", "%.2f m/s", s.speed);
        ImGui::EndTable();
    }
    GameSection(s);
    ImGui::SeparatorText("Partner");
    if (BeginProps("partner")) {
        Prop("entity", "%p", reinterpret_cast<void*>(s.partner_entity));
        Prop("state", "%s%s%s", s.partner_state, s.spawn_held ? "  (held by Remove)" : "",
             s.spawn_disabled ? "  (spawning disabled this run)" : "");
        if (s.partner_source[0]) Prop("source", "%s", s.partner_source);
        Prop("flags", "0x%llX", static_cast<unsigned long long>(s.partner_flags));
        if (s.partner_pos_valid) {
            Prop("position", "%.2f  %.2f  %.2f", s.partner_pos[0], s.partner_pos[1], s.partner_pos[2]);
            if (s.local_valid) Prop("distance", "%.1f m", Dist(s.partner_pos, s.local_pos));
        }
        if (s.remote_age_ms >= 0) Prop("remote pose", "%s, %.0f ms ago", s.remote_fresh ? "fresh" : "stale", s.remote_age_ms);
        else Prop("remote pose", "none yet");
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Rates");
    if (BeginProps("rates")) {
        Prop("tick", "%.1f Hz (%llu ticks)", s.tick_hz, static_cast<unsigned long long>(s.ticks));
        Prop("present", "%.1f fps", c.stats->fps);
        Prop("send", "%u Hz, interp %u ms", s.send_hz, s.interp_delay_ms);
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ActionButtons(debug::Tab::status);
}

void PartnerTab(const Context& c) {
    const debug::Snapshot& s = *c.snap;
    Text(&kDim, "state");
    ImGui::SameLine();
    ImGui::TextColored(std::strcmp(s.partner_state, "visible") == 0 ? kGreen : kAmber, "%s", s.partner_state);
    if (s.partner_pos_valid && s.local_valid) {
        ImGui::SameLine();
        Text(&kDim, "  %.1f m away", Dist(s.partner_pos, s.local_pos));
    }
    if (s.spawn_held) Text(&kAmber, "Spawning is held (Remove partner). Respawn lifts the hold.");
    ImGui::Spacing();
    ActionButtons(debug::Tab::partner);

    ImGui::SeparatorText("Echo tuning (echo role)");
    if (!g_echo_init && s.serial) {
        g_echo_delay = s.echo_delay_s;
        g_echo_offset = s.echo_offset_m;
        g_echo_init = true;
    }
    const bool echo = std::strcmp(s.role, "echo") == 0;
    ImGui::BeginDisabled(!echo);
    bool changed = false;
    ImGui::SliderFloat("delay (s)", &g_echo_delay, 0.1f, 15.0f, "%.2f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SliderFloat("sideways offset (m)", &g_echo_offset, -10.0f, 10.0f, "%.2f");
    changed |= ImGui::IsItemDeactivatedAfterEdit();
    if (changed) {
        debug::Request r;
        r.action = debug::Action::set_echo_tuning;
        r.f[0] = g_echo_delay;
        r.f[1] = g_echo_offset;
        Send(r);
    }
    Chip(debug::Action::set_echo_tuning);
    ImGui::EndDisabled();
    if (!echo) Text(&kDim, "Only used when role=echo.");
    else Text(&kDim, "Live: %.2f s, %.2f m", s.echo_delay_s, s.echo_offset_m);
}

void PlayerTab(const Context& c) {
    const debug::Snapshot& s = *c.snap;
    ImGui::SeparatorText("Position");
    if (s.local_valid) {
        Text(nullptr, "%.3f  %.3f  %.3f", s.local_pos[0], s.local_pos[1], s.local_pos[2]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(FormatCoordinates(s.local_pos[0], s.local_pos[1], s.local_pos[2]).c_str());
        }
        Tooltip("Copy \"x y z\" to the clipboard.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Use as target")) {
            g_target[0] = s.local_pos[0], g_target[1] = s.local_pos[1], g_target[2] = s.local_pos[2];
        }
    } else {
        Text(&kAmber, "No player position (title screen, loading or cutscene).");
    }

    ImGui::SeparatorText("Teleport");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
    ImGui::InputScalarN("x y z", ImGuiDataType_Double, g_target, 3, nullptr, nullptr, "%.2f");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
    if (ImGui::InputTextWithHint("##paste", "paste \"x y z\" and press Enter", g_paste, sizeof(g_paste),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        double v[3];
        if (ParseCoordinates(g_paste, v)) {
            g_target[0] = v[0], g_target[1] = v[1], g_target[2] = v[2];
            g_paste[0] = 0;
        }
    }
    if (ImGui::Button("Teleport to coordinates")) TeleportTo(g_target[0], g_target[1], g_target[2]);
    Tooltip("PlayerGame::PlaceOn mode 1 at x y z (short ground probe), on the game thread.");
    ImGui::SameLine();
    if (ImGui::Button("Snap to ground (x, y)"))
        TeleportTo(g_target[0], g_target[1], game::kSnapRayCentreZ, game::TeleportMode::snap_to_ground);
    Tooltip("PlaceOn mode 2: ignores z; the game drops Sam onto the topmost surface at x, y (may be a roof).");
    ImGui::SameLine();
    if (const debug::ActionInfo* a = debug::Find(debug::Action::teleport_to_partner)) {
        ImGui::BeginDisabled(!s.partner_pos_valid && !s.remote_fresh);
        if (ImGui::Button(a->label)) Send(a->id);
        Tooltip(a->help);
        ImGui::EndDisabled();
    }
    Chip(debug::Action::teleport);
    Chip(debug::Action::teleport_to_partner);
    if (!s.diag.teleport_ready && s.diag.resolved) Text(&kAmber, "Teleport blocked now: %s", s.diag.teleport_block);
    if (s.diag.last_teleport[0]) Text(&kDim, "Last: %s", s.diag.last_teleport);

    ImGui::SeparatorText("Saved positions");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::InputTextWithHint("##name", "name", g_new_name, sizeof(g_new_name));
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.local_valid);
    if (ImGui::Button("Save current position")) {
        SavedPosition p;
        p.name = CleanName(g_new_name, "position " + std::to_string(g_positions.size() + 1));
        p.x = s.local_pos[0], p.y = s.local_pos[1], p.z = s.local_pos[2];
        g_positions.push_back(p);
        g_selected = static_cast<int>(g_positions.size()) - 1;
        g_new_name[0] = 0;
        SavePositionsFile();
    }
    ImGui::EndDisabled();
    if (ImGui::BeginListBox("##positions", ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 7))) {
        for (int i = 0; i < static_cast<int>(g_positions.size()); ++i) {
            const SavedPosition& p = g_positions[static_cast<std::size_t>(i)];
            char label[160];
            std::snprintf(label, sizeof(label), "%s   (%.1f, %.1f, %.1f)##%d", p.name.c_str(), p.x, p.y, p.z, i);
            if (ImGui::Selectable(label, g_selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                g_selected = i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) TeleportTo(p.x, p.y, p.z);
            }
        }
        ImGui::EndListBox();
    }
    const bool sel = g_selected >= 0 && g_selected < static_cast<int>(g_positions.size());
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Teleport to saved position") && sel) {
        const SavedPosition& p = g_positions[static_cast<std::size_t>(g_selected)];
        TeleportTo(p.x, p.y, p.z);
    }
    ImGui::SameLine();
    if (ImGui::Button("Set as target") && sel) {
        const SavedPosition& p = g_positions[static_cast<std::size_t>(g_selected)];
        g_target[0] = p.x, g_target[1] = p.y, g_target[2] = p.z;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy##saved") && sel) {
        const SavedPosition& p = g_positions[static_cast<std::size_t>(g_selected)];
        ImGui::SetClipboardText(FormatCoordinates(p.x, p.y, p.z).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && sel) {
        g_positions.erase(g_positions.begin() + g_selected);
        g_selected = -1;
        SavePositionsFile();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        LoadPositions(g_positions_path, g_positions);
        g_selected = -1;
        std::snprintf(g_positions_note, sizeof(g_positions_note), "loaded %zu position(s)", g_positions.size());
    }
    Tooltip("Re-read positions.txt (edit it by hand: \"x y z name\" per line).");
    if (g_positions_note[0]) Text(&kDim, "%s", g_positions_note);

    ImGui::SeparatorText("Time of day");
    const game::Diagnostics& d = s.diag;
    if (!g_tod_init && d.tod_valid) {
        g_tod_hours = d.time_of_day;
        g_tod_init = true;
    }
    ImGui::BeginDisabled(!d.tod_valid);
    char clock[8];
    game::FormatClock(g_tod_hours, clock, sizeof(clock));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    ImGui::SliderFloat("##tod", &g_tod_hours, 0.0f, 23.99f, clock);
    ImGui::SameLine();
    if (ImGui::Button("Apply##tod")) {
        debug::Request r;
        r.action = debug::Action::set_time_of_day;
        r.f[0] = g_tod_hours;
        Send(r);
    }
    Tooltip("GameWorldTimeState::SetTimeOfDay, instant.");
    ImGui::SameLine();
    bool frozen = d.tod_valid && !d.day_night_cycle;
    if (ImGui::Checkbox("Freeze time", &frozen)) {
        debug::Request r;
        r.action = debug::Action::set_day_night_cycle;
        r.flag = !frozen;
        Send(r);
    }
    Tooltip("EnableDayNightCycle off/on.");
    ImGui::EndDisabled();
    if (d.tod_valid) {
        game::FormatClock(d.time_of_day, clock, sizeof(clock));
        Text(&kDim, "Now %s, day %d, cycle %s", clock, d.day, d.day_night_cycle ? "on" : "frozen");
    } else {
        Text(&kDim, "No time of day (title screen, loading or disabled).");
    }
    Chip(debug::Action::set_time_of_day);
    Chip(debug::Action::set_day_night_cycle);

    ImGui::SeparatorText("More");
    if (d.health_valid) Text(&kDim, "Health %.1f / %.1f", static_cast<double>(d.health), static_cast<double>(d.health_max));
    ActionButtons(debug::Tab::player);
}

bool MatchesFilter(const char* line, const char* filter) {
    if (!filter[0]) return true;
    for (const char* p = line; *p; ++p) {
        const char* a = p;
        const char* b = filter;
        while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b))) ++a, ++b;
        if (!*b) return true;
    }
    return false;
}

void LogTab() {
    constexpr std::size_t kKeep = 300;
    if (g_fetch.size() != kKeep) g_fetch.resize(kKeep);
    const std::size_t n = LogCopyRecent(g_last_seq, g_fetch.data(), kKeep);
    for (std::size_t i = 0; i < n; ++i) {
        g_lines.push_back(g_fetch[i]);
        g_last_seq = g_fetch[i].seq;
    }
    if (g_lines.size() > kKeep) g_lines.erase(g_lines.begin(), g_lines.begin() + static_cast<std::ptrdiff_t>(g_lines.size() - kKeep));

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
    ImGui::InputTextWithHint("##filter", "filter", g_filter, sizeof(g_filter));
    ImGui::SameLine();
    ImGui::Checkbox("auto-scroll", &g_autoscroll);
    std::vector<int> shown;
    shown.reserve(g_lines.size());
    for (int i = 0; i < static_cast<int>(g_lines.size()); ++i)
        if (MatchesFilter(g_lines[static_cast<std::size_t>(i)].text, g_filter)) shown.push_back(i);
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string all;
        for (int i : shown) {
            all += g_lines[static_cast<std::size_t>(i)].text;
            all += "\r\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }
    Tooltip("Copy the lines shown (after the filter).");
    ImGui::SameLine();
    if (ImGui::Button("Clear view")) g_lines.clear();
    ImGui::SameLine();
    Text(&kDim, "%zu / %zu lines", shown.size(), g_lines.size());

    if (ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(shown.size()));
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const char* t = g_lines[static_cast<std::size_t>(shown[static_cast<std::size_t>(r)])].text;
                const bool bad = std::strstr(t, "FAIL") || std::strstr(t, "EXCEPTION") || std::strstr(t, "DISABLED");
                if (bad) ImGui::TextColored(kRed, "%s", t);
                else ImGui::TextUnformatted(t);
            }
        if (g_autoscroll && n > 0) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void SettingsTab(Context& c) {
    const debug::Snapshot& s = *c.snap;
    ImGui::SeparatorText("Logging");
    bool verbose = s.verbose;
    if (ImGui::Checkbox("Verbose logging", &verbose)) {
        debug::Request r;
        r.action = debug::Action::set_verbose;
        r.flag = verbose;
        Send(r);
    }
    Tooltip("Adds a [sync] trace line every 2 s (session.ini: verbose=1).");
    Chip(debug::Action::set_verbose);

    ImGui::SeparatorText("Menu");
    if (c.ui_scale) {
        float scale = *c.ui_scale;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        if (ImGui::SliderFloat("UI scale", &scale, 0.75f, 3.0f, "%.2f")) *c.ui_scale = scale;
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto")) *c.ui_scale = 0;
    }
    char key[24];
    Text(nullptr, "Menu key: %s (VK 0x%02X). Change it with debug_menu_key=F10 in session.ini.",
         VkName(c.toggle_vk, key, sizeof(key)), c.toggle_vk);
    Text(&kDim, "Esc or the key closes the menu. While it is open the game gets no mouse, keyboard or XInput pad.");

    ImGui::SeparatorText("Tools");
    ActionButtons(debug::Tab::tools);

    ImGui::SeparatorText("Recent actions");
    debug::Result recent[8];
    const std::size_t n = debug::RecentResults(recent, 8);
    if (!n) Text(&kDim, "none yet");
    for (std::size_t i = n; i-- > 0;)
        Text(recent[i].ok ? &kGreen : &kRed, "#%u %s: %s", recent[i].serial, debug::ActionName(recent[i].action), recent[i].text);

    if (ImGui::CollapsingHeader("Overlay diagnostics")) {
        const Stats& st = *c.stats;
        if (BeginProps("diag")) {
            Prop("back buffer", "%u x %u  %s", st.width, st.height, FormatName(st.format));
            Prop("queue", "%s", st.queue_source);
            Prop("frames", "drawn %lu / presents %lu, resizes %lu, renderer inits %lu / releases %lu", st.frames_drawn,
                 st.presents, st.resizes, st.renderer_inits, st.renderer_releases);
            Prop("last frame", "%d vertices, %d draw lists", st.last_vertices, st.last_draw_lists);
            Prop("input", "window %s, keyboard hook %s, cursor hooks %s, XInput hook %s", c.input.subclassed ? "yes" : "no",
                 c.input.keyboard_hook ? "yes" : "no", c.input.cursor_hooks ? "yes" : "no", c.input.xinput_hook ? "yes" : "no");
            Prop("blocked", "%lu keys, %lu messages, %lu raw input, %lu idle pads", c.input.blocked_keys, c.input.blocked_messages,
                 c.input.blocked_raw_input, c.input.idle_pads);
            Prop("suppressed", "%lu ClipCursor, %lu SetCursorPos", c.input.suppressed_clip, c.input.suppressed_cursor_pos);
            ImGui::EndTable();
        }
    }
}

}  // namespace

void Init(const std::wstring& mod_dir) {
    g_positions_path = mod_dir.empty() ? std::wstring() : mod_dir + L"\\positions.txt";
    if (!g_positions_path.empty() && LoadPositions(g_positions_path, g_positions))
        Log("[overlay] %zu saved position(s) loaded", g_positions.size());
}

void SelectTab(int index) { g_select_tab = index; }

void Draw(Context& c) {
    const float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(fs * 2, fs * 2), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(fs * 40, fs * 34), ImGuiCond_FirstUseEver);
    char title[96];
    std::snprintf(title, sizeof(title), "Stranded Together %s###st_debug_menu", c.build);
    bool open = true;
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse)) {
        Header(c);
        if (c.coop) {  // the host-held save session (docs/coop-session.md)
            Text(c.coop->banner_warn ? &kAmber : &kDim, "Co-op save: %s", c.coop->status[0] ? c.coop->status : "-");
            if (c.coop->teleport_offer) {
                if (ImGui::Button("Teleport to host")) c.coop_teleport = true;
                Tooltip("You joined while the host was playing: go to where the host is now (once).");
            }
        }
        if (ImGui::BeginTabBar("tabs")) {
            const auto flags = [](int i) {
                return g_select_tab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            };
            if (ImGui::BeginTabItem("Status", nullptr, flags(0))) {
                StatusTab(c);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Partner", nullptr, flags(1))) {
                PartnerTab(c);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Player", nullptr, flags(2))) {
                PlayerTab(c);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Log", nullptr, flags(3))) {
                LogTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings", nullptr, flags(4))) {
                SettingsTab(c);
                ImGui::EndTabItem();
            }
            g_select_tab = -1;
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    if (!open || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive())) c.close = true;
}

}  // namespace st::overlay::menu
