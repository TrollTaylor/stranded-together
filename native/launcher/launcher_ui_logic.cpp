// Stranded Together: pure GUI logic (see launcher_ui_logic.hpp).
#include "launcher_ui_logic.hpp"

#include <cstdio>
#include <cwchar>

namespace st::ui {

const char* PageLabel(Page page) {
    switch (page) {
    case Page::play: return "Play";
    case Page::in_game: return "In game";
    case Page::tools: return "Tools";
    case Page::debug: return "Debug";
    case Page::about: return "About";
    }
    return "";
}

bool SessionActive(Phase phase) {
    return phase == Phase::launching || phase == Phase::in_game || phase == Phase::after_game;
}

bool CanStartSession(Phase phase) { return phase == Phase::ready; }

bool CanHost(Phase phase, const InstallChecks& checks) { return CanStartSession(phase) && checks.steam_running; }

bool PageVisible(Phase phase, Page) { return phase != Phase::not_in_game_folder; }

bool PageEnabled(Phase phase, Page page) {
    if (phase == Phase::not_in_game_folder) return false;
    switch (page) {
    case Page::play: return true;
    case Page::in_game: return SessionActive(phase);
    case Page::tools:
    case Page::debug:
    case Page::about: return true;
    }
    return false;
}

Page DefaultPage(Phase phase) { return SessionActive(phase) ? Page::in_game : Page::play; }

Page PageAfterPhaseChange(Phase from, Phase to, Page current) {
    Page next = current;
    const bool side_page = current == Page::tools || current == Page::debug || current == Page::about;
    if (from != to) {
        switch (to) {
        case Phase::not_in_game_folder: next = Page::play; break;
        case Phase::launching:
        case Phase::in_game:
        case Phase::after_game:
            if (current == Page::play) next = Page::in_game;
            break;
        case Phase::ready:
            if (current == Page::in_game) next = Page::play;
            break;
        case Phase::error:
            if (!side_page) next = Page::play;
            break;
        }
    }
    if (!PageEnabled(to, next)) next = DefaultPage(to);
    return next;
}

const char* PlayTabLabel(PlayTab tab) {
    switch (tab) {
    case PlayTab::host: return "Host";
    case PlayTab::join: return "Join";
    case PlayTab::solo: return "Solo test";
    }
    return "";
}

PlayTab TabForRole(Role role) {
    switch (role) {
    case Role::host: return PlayTab::host;
    case Role::join: return PlayTab::join;
    case Role::echo:
    case Role::solo: return PlayTab::solo;
    }
    return PlayTab::host;
}

const char* DebugTabLabel(DebugTab tab) {
    switch (tab) {
    case DebugTab::options: return "Options";
    case DebugTab::saves: return "Test saves";
    case DebugTab::files: return "Files and log";
    }
    return "";
}

const char* FunctionKeyItems() { return "F1\0F2\0F3\0F4\0F5\0F6\0F7\0F8\0F9\0F10\0F11\0F12\0\0"; }

std::string ModChipText(const InstallChecks& c) {
    switch (c.mod_state) {
    case ModState::ok: return c.package_version.empty() ? std::string("Mod installed") : "Mod " + c.package_version;
    case ModState::dev_build: return "Mod: development build";
    case ModState::mismatch: return "Mod: version mismatch";
    case ModState::foreign: return "winhttp.dll is not ours";
    case ModState::disabled: return "Mod switched off";
    case ModState::missing: break;
    }
    return "Mod missing";
}

Tone ModTone(ModState state) {
    switch (state) {
    case ModState::ok: return Tone::good;
    case ModState::dev_build: return Tone::neutral;
    case ModState::mismatch:
    case ModState::disabled: return Tone::warn;
    case ModState::foreign:
    case ModState::missing: break;
    }
    return Tone::bad;
}

std::string LastLines(const std::string& text, std::size_t max_lines) {
    if (max_lines == 0) return {};
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    std::size_t pos = end, lines = 0;
    while (pos > 0) {
        if (text[pos - 1] == '\n' && ++lines == max_lines) break;
        --pos;
    }
    std::string out;
    out.reserve(end - pos);
    for (std::size_t i = pos; i < end; ++i)
        if (text[i] != '\r') out.push_back(text[i]);
    return out;
}

JoinFieldView EvaluateJoinField(const std::string& text, const CodeCheck& check, bool busy) {
    JoinFieldView view;
    if (busy) {
        view.field_enabled = false;
        view.message = "A game is already running. Close it to join someone else.";
        return view;
    }
    bool blank = true;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') blank = false;
    if (blank) {
        view.message = "Paste the code your partner sent you. A Steam invite (+stranded_join ...) works too.";
        return view;
    }
    view.can_join = check.ok;
    view.tone = check.ok ? Tone::good : Tone::bad;
    view.message = check.message.empty() ? (check.ok ? "The code looks right." : "That code doesn't work.") : check.message;
    return view;
}

namespace {
bool IsDigitW(char c) { return c >= '0' && c <= '9'; }

bool CanonicalIPv4(const std::string& text) {
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        const std::size_t start = i;
        int octet = 0;
        while (i < text.size() && IsDigitW(text[i]) && i - start < 3) octet = octet * 10 + (text[i++] - '0');
        if (i == start || octet > 255 || (i - start > 1 && text[start] == '0')) return false;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    return i == text.size();
}
}  // namespace

bool ParsePort(const std::string& text, int& out) {
    if (text.empty() || text.size() > 5) return false;
    int v = 0;
    for (char c : text) {
        if (!IsDigitW(c)) return false;
        v = v * 10 + (c - '0');
    }
    if (v < 1 || v > 65535) return false;
    out = v;
    return true;
}

std::string LanProblem(const LanSettings& lan) {
    if (lan.peer_ip.empty()) return "Enter your partner's IP address (for example 192.168.1.20).";
    if (!CanonicalIPv4(lan.peer_ip)) return "That is not an IPv4 address. Use four numbers with dots, such as 192.168.1.20.";
    if (lan.port_local < 1 || lan.port_local > 65535 || lan.port_peer < 1 || lan.port_peer > 65535)
        return "The ports must be numbers from 1 to 65535.";
    return {};
}

CodeDisplay SplitJoinCode(const std::string& code) {
    CodeDisplay out;
    const std::size_t at = code.rfind('@');
    if (at == std::string::npos) {
        out.main = code;
    } else {
        out.main = code.substr(0, at);
        out.account = code.substr(at);
    }
    return out;
}

Tone PingTone(int ms) {
    if (ms <= 0) return Tone::neutral;
    if (ms < 90) return Tone::good;
    if (ms < 180) return Tone::warn;
    return Tone::bad;
}

std::string FormatPing(int ms) {
    if (ms <= 0) return "-";
    return std::to_string(ms) + " ms";
}

std::string RouteText(const std::string& transport, const std::string& route) {
    if (transport == "udp") return "LAN (direct)";
    if (route == "direct") return "Steam (direct)";
    if (route == "relay") return "Steam relay";
    if (transport == "steam") return "Steam";
    return "-";
}

std::string RemoteText(const std::string& remote) {
    if (remote == "visible") return "Visible";
    if (remote == "spawning") return "Appearing...";
    if (remote == "failed") return "Could not appear";
    if (remote == "none") return "Not in your world yet";
    return "-";
}

namespace {
const char* const kSceneNames[static_cast<int>(Scene::count)] = {
    "01-play-host",           "02-play-host-launching", "03-play-join",           "04-play-join-invalid",
    "05-play-join-valid",     "06-play-solo",           "07-play-lan",            "08-play-untested-version",
    "09-play-steam-offline",  "10-play-dev-build",      "11-play-mod-mismatch",   "12-not-installed",
    "13-mod-foreign",         "14-mod-disabled",        "15-wrong-folder",        "16-ingame-launching",
    "17-ingame-host-waiting", "18-ingame-connected",    "19-ingame-echo",         "20-ingame-solo",
    "21-ingame-not-responding", "22-ingame-lost",       "23-ingame-after-game",   "24-tools",
    "25-tools-result",        "26-tools-in-game",       "27-debug-options",       "28-debug-files",
    "29-debug-in-game",       "30-about",               "31-play-update-available", "32-play-update-downloading",
    "33-play-update-failed",  "34-play-updated",        "35-about-updates",       "36-debug-test-saves",
    "37-debug-local-test",    "38-play-join-characters", "39-play-join-hologram",   "40-debug-options-characters",
    "41-debug-options-coop",  "42-debug-local-test-off",
};
}  // namespace

int SceneCount() { return static_cast<int>(Scene::count); }

const char* SceneName(Scene scene) {
    const int i = static_cast<int>(scene);
    return i >= 0 && i < SceneCount() ? kSceneNames[i] : "";
}

int IdleWaitMs(Phase phase, bool animating) {
    if (animating) return 16;
    switch (phase) {
    case Phase::launching:
    case Phase::after_game: return 100;
    case Phase::in_game: return 250;
    default: return 1000;
    }
}

namespace {
bool Is(const std::wstring& a, const wchar_t* b) { return a == b; }

bool ParseNumber(const std::wstring& text, double lo, double hi, double& out) {
    if (text.empty() || text.size() > 16) return false;
    wchar_t* end = nullptr;
    const double v = std::wcstod(text.c_str(), &end);
    if (!end || *end != L'\0' || !(v >= lo && v <= hi)) return false;
    out = v;
    return true;
}

bool ParsePortW(const std::wstring& text, int& out) {
    std::string narrow;
    for (wchar_t c : text) {
        if (c > 0x7F) return false;
        narrow.push_back(static_cast<char>(c));
    }
    return ParsePort(narrow, out);
}

bool ValidSession(const std::wstring& s) {
    if (s.empty() || s.size() > 64) return false;
    for (wchar_t c : s)
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'-' || c == L'_'))
            return false;
    return true;
}
}  // namespace

// ---- Updates -----------------------------------------------------------------
UpdateBanner UpdateBannerFor(const UpdateInfo& info) {
    switch (info.status) {
    case UpdateStatus::available: return UpdateBanner::available;
    case UpdateStatus::downloading: return UpdateBanner::downloading;
    case UpdateStatus::applying:
    case UpdateStatus::ready_to_restart: return UpdateBanner::installing;
    case UpdateStatus::error:
        if (!info.error_after_check && !info.latest_version.empty()) return UpdateBanner::failed;
        break;
    case UpdateStatus::idle:
    case UpdateStatus::checking:
    case UpdateStatus::up_to_date: break;
    }
    return info.updated_to.empty() ? UpdateBanner::none : UpdateBanner::updated;
}

bool CanStartUpdate(Phase phase, const UpdateInfo& info) {
    if (phase == Phase::not_in_game_folder || SessionActive(phase)) return false;
    if (info.download_url.empty() || info.latest_version.empty()) return false;
    return info.status == UpdateStatus::available || (info.status == UpdateStatus::error && !info.error_after_check);
}

bool CanCheckForUpdates(const UpdateInfo& info) {
    return info.status != UpdateStatus::checking && !UpdateInProgress(info);
}

bool UpdateInProgress(const UpdateInfo& info) {
    return info.status == UpdateStatus::downloading || info.status == UpdateStatus::applying || info.status == UpdateStatus::ready_to_restart;
}

bool CloseForUpdate(const UpdateInfo& info) { return info.status == UpdateStatus::ready_to_restart; }

std::string UpdateStatusText(const UpdateInfo& info) {
    switch (info.status) {
    case UpdateStatus::idle: return info.updated_to.empty() ? "Not checked yet." : "Updated to version " + info.updated_to + ".";
    case UpdateStatus::checking: return "Checking for updates...";
    case UpdateStatus::up_to_date:
        if (!info.updated_to.empty()) return "Updated to version " + info.updated_to + ". You have the latest version.";
        return "You have the latest version.";
    case UpdateStatus::available: return "Version " + info.latest_version + " is available.";
    case UpdateStatus::downloading: return "Downloading version " + info.latest_version + "...";
    case UpdateStatus::applying: return "Installing version " + info.latest_version + "...";
    case UpdateStatus::ready_to_restart: return "Restarting into version " + info.latest_version + "...";
    case UpdateStatus::error: return info.error.empty() ? std::string("The update did not go through.") : info.error;
    }
    return {};
}

std::string NotesExcerpt(const std::string& notes, std::size_t max_lines, std::size_t max_chars) {
    std::vector<std::string> lines;
    std::size_t total = 0;
    bool cut = false;
    std::size_t pos = 0;
    while (pos <= notes.size()) {
        std::size_t end = notes.find('\n', pos);
        if (end == std::string::npos) end = notes.size();
        std::string line = notes.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::size_t a = line.find_first_not_of(" \t");
        line = a == std::string::npos ? std::string() : line.substr(a);
        if (!line.empty() && line[0] == '#') continue;   // headings ("## What's new") say nothing on their own
        if (line.size() >= 2 && (line[0] == '*' || line[0] == '-' || line[0] == '+') && line[1] == ' ') line = "- " + line.substr(2);
        std::string plain;
        for (char c : line)
            if (c != '*' && c != '`') plain.push_back(c);
        a = plain.find_first_not_of(" \t");
        plain = a == std::string::npos ? std::string() : plain.substr(a);
        while (!plain.empty() && (plain.back() == ' ' || plain.back() == '\t')) plain.pop_back();
        if (plain.empty()) continue;
        if (lines.size() == max_lines) {
            cut = true;
            break;
        }
        if (total + plain.size() > max_chars) {
            std::size_t room = max_chars > total ? max_chars - total : 0;
            // Never cut a UTF-8 character in half (a continuation byte is 10xxxxxx).
            while (room > 0 && (static_cast<unsigned char>(plain[room]) & 0xC0) == 0x80) --room;
            if (room > 0) lines.push_back(plain.substr(0, room));
            cut = true;
            break;
        }
        total += plain.size();
        lines.push_back(plain);
        if (end == notes.size()) break;
    }
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) out += (i ? "\n" : "") + lines[i];
    if (cut && !out.empty()) {
        while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
        out += "...";
    }
    return out;
}

std::string FormatBytes(std::uint64_t bytes) {
    char buf[32];
    if (bytes >= 1024ull * 1024ull) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024ull) std::snprintf(buf, sizeof(buf), "%llu KB", static_cast<unsigned long long>((bytes + 512) / 1024));
    else std::snprintf(buf, sizeof(buf), "%llu bytes", static_cast<unsigned long long>(bytes));
    return buf;
}

namespace {
bool ParsePid(const std::wstring& text, std::uint32_t& out) {
    if (text.empty() || text.size() > 10) return false;
    unsigned long long v = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        v = v * 10 + static_cast<unsigned long long>(c - L'0');
    }
    if (v == 0 || v > 0xFFFFFFFFull) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

// http(s)://127.0.0.1[:port]/... or http(s)://localhost[:port]/... (no user info, no other host).
bool LoopbackFeed(const std::wstring& url) {
    std::wstring lower;
    for (wchar_t c : url) lower.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    std::wstring rest;
    if (lower.rfind(L"http://", 0) == 0) rest = lower.substr(7);
    else if (lower.rfind(L"https://", 0) == 0) rest = lower.substr(8);
    else return false;
    const std::wstring authority = rest.substr(0, rest.find_first_of(L"/?#"));
    if (authority.find(L'@') != std::wstring::npos) return false;
    std::wstring host = authority;
    const std::size_t colon = authority.rfind(L':');
    if (colon != std::wstring::npos) {
        const std::wstring port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
        host = authority.substr(0, colon);
    }
    return host == L"127.0.0.1" || host == L"localhost";
}
}  // namespace

Options ParseCommandLine(const std::vector<std::wstring>& args) {
    Options o;
    int modes = 0;
    bool session_extras = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        const bool has_value = i + 1 < args.size();
        if (Is(a, L"--mock")) {
            o.mock = true;
        } else if (Is(a, L"--watch")) {
            if (!has_value || !ValidSession(args[i + 1])) { o.error = L"--watch needs a session id."; return o; }
            o.mode = Options::Mode::watch;
            o.watch_session = args[++i];
            ++modes;
        } else if (Is(a, L"--selftest")) {
            o.mode = Options::Mode::selftest;
            ++modes;
        } else if (Is(a, L"--screenshot-all")) {
            if (!has_value || args[i + 1].empty()) { o.error = L"--screenshot-all needs a folder."; return o; }
            o.mode = Options::Mode::screenshots;
            o.screenshot_dir = args[++i];
            ++modes;
        } else if (Is(a, L"--mock-scenario")) {
            if (!has_value || args[i + 1].empty()) { o.error = L"--mock-scenario needs a name."; return o; }
            o.scenario = args[++i];
            o.mock = true;
        } else if (Is(a, L"--mock-speed")) {
            if (!has_value || !ParseNumber(args[i + 1], 0.1, 100.0, o.mock_speed)) {
                o.error = L"--mock-speed needs a number from 0.1 to 100.";
                return o;
            }
            o.mock = true;
            ++i;
        } else if (Is(a, L"--scale")) {
            double v = 0.0;
            if (!has_value || !ParseNumber(args[i + 1], 0.5, 4.0, v)) { o.error = L"--scale needs a number from 0.5 to 4."; return o; }
            o.scale = static_cast<float>(v);
            ++i;
        } else if (Is(a, L"--help") || Is(a, L"-h") || Is(a, L"/?")) {
            o.mode = Options::Mode::help;
            ++modes;
        } else if (Is(a, L"--write-session")) {
            if (!has_value ||
                (!Is(args[i + 1], L"host") && !Is(args[i + 1], L"join") && !Is(args[i + 1], L"echo") && !Is(args[i + 1], L"solo") &&
                 !Is(args[i + 1], L"local-test"))) {
                o.error = L"--write-session needs host, join, echo, solo or local-test.";
                return o;
            }
            o.mode = Options::Mode::write_session;
            o.write_role = args[++i];
            ++modes;
        } else if (Is(a, L"--end-session")) {
            o.mode = Options::Mode::end_session;
            ++modes;
        } else if (Is(a, L"--out")) {
            if (!has_value || args[i + 1].empty()) { o.error = L"--out needs a folder."; return o; }
            o.write_out = args[++i];
            session_extras = true;
        } else if (Is(a, L"--code")) {
            if (!has_value || args[i + 1].empty()) { o.error = L"--code needs the host's join code."; return o; }
            o.write_code = args[++i];
            session_extras = true;
        } else if (Is(a, L"--transport")) {
            if (!has_value || (!Is(args[i + 1], L"steam") && !Is(args[i + 1], L"udp"))) { o.error = L"--transport needs steam or udp."; return o; }
            o.write_transport = args[++i];
            session_extras = true;
        } else if (Is(a, L"--udp-peer")) {
            if (!has_value || args[i + 1].empty()) { o.error = L"--udp-peer needs an IPv4 address."; return o; }
            o.write_udp_peer = args[++i];
            session_extras = true;
        } else if (Is(a, L"--udp-port-local") || Is(a, L"--udp-port-peer")) {
            int port = 0;
            if (!has_value || !ParsePortW(args[i + 1], port)) { o.error = a + L" needs a port from 1 to 65535."; return o; }
            (Is(a, L"--udp-port-local") ? o.write_port_local : o.write_port_peer) = port;
            ++i;
            session_extras = true;
        } else if (Is(a, L"--after-update")) {
            if (!has_value || !ParsePid(args[i + 1], o.after_update)) { o.error = L"--after-update needs a process id."; return o; }
            ++i;
        } else if (Is(a, L"--update-feed")) {
            if (!has_value || !LoopbackFeed(args[i + 1])) {
                o.error = L"--update-feed needs a test feed on this PC: http://127.0.0.1:<port>/... or http://localhost:<port>/...";
                return o;
            }
            o.update_feed = args[++i];
        } else if (Is(a, L"--update-now")) {
            o.update_now = true;
        } else if (Is(a, L"--game-dir")) {
            if (!has_value || args[i + 1].empty()) { o.error = L"--game-dir needs a folder."; return o; }
            o.game_dir = args[++i];
        } else {
            o.error = L"Unknown option: " + a;
            return o;
        }
    }
    if (modes > 1) {
        o.error = L"Use only one of --watch, --selftest, --screenshot-all, --write-session, --end-session and --help.";
        return o;
    }
    const bool session_flag = o.mode == Options::Mode::write_session || o.mode == Options::Mode::end_session;
    if (!o.game_dir.empty() && !session_flag) o.error = L"--game-dir goes with --write-session or --end-session.";
    else if (session_extras && o.mode != Options::Mode::write_session)
        o.error = L"--code, --out, --transport, --udp-peer and --udp-port-* go with --write-session.";
    else if (o.mode == Options::Mode::write_session && o.write_role == L"join" && o.write_code.empty())
        o.error = L"--write-session join needs --code <the host's code>.";
    else if (o.mode == Options::Mode::write_session && Is(o.write_role, L"local-test") && o.write_out.empty())
        o.error = L"--write-session local-test needs --out <folder> (it never writes into the game folder).";
    else if (o.mode == Options::Mode::write_session && !Is(o.write_role, L"local-test") && !o.write_out.empty())
        o.error = L"--out goes with --write-session local-test.";
    else if (session_flag && o.mock) o.error = L"--mock does not go with --write-session or --end-session.";
    else if ((o.after_update || !o.update_feed.empty() || o.update_now) && (o.mode != Options::Mode::gui || o.mock))
        o.error = L"--after-update, --update-feed and --update-now go with the window on the real model (no --mock).";
    else if (o.update_now && o.update_feed.empty())
        o.error = L"--update-now needs --update-feed: it installs from a test feed only.";
    return o;
}

const wchar_t* UsageText() {
    return L"StrandedTogether.exe [options]\n"
           L"  (no options)              open the launcher window\n"
           L"  --mock                    use the simulated model (developer check, no game)\n"
           L"  --mock-scenario <name>    ready | wrong-folder | not-installed | untested-version |\n"
           L"                            steam-offline | partner-drops | dev-build | mod-mismatch |\n"
           L"                            mod-foreign | mod-disabled | update-available\n"
           L"  --mock-speed <factor>     run the simulation faster or slower\n"
           L"  --scale <factor>          force a UI scale instead of the monitor's DPI\n"
           L"  --screenshot-all <dir>    render every page once (hidden) and save PNGs\n"
           L"  --selftest                run the built-in checks; exit code 0 = passed\n"
           L"  --watch <session>         background helper while the game runs (no window)\n"
           L"Developer only (no window, no Steam launch, no helper):\n"
           L"  --write-session host|join|echo|solo   write session.ini exactly as Host / Join / Solo test /\n"
           L"                            Debug tools only (with the Debug page's options, one_shot=1)\n"
           L"    --code <code>           join: the host's DST1 code\n"
           L"    --transport steam|udp   default steam (echo: loopback udp)\n"
           L"    --udp-peer <ip> --udp-port-local <n> --udp-port-peer <n>\n"
           L"    --game-dir <dir>        the game folder (default: this program's folder)\n"
           L"  --write-session local-test --out <dir>   the local two-player test's host.ini + guest.ini, exactly as\n"
           L"                            Debug > Local test writes them, into <dir> (testenv -LauncherIni)\n"
           L"  --end-session [--game-dir <dir>]  remove session.ini again (not while the game runs)\n"
           L"  --update-feed <url>       read releases from a test feed on this PC (http://127.0.0.1:<port>/...)\n"
           L"  --update-now              with --update-feed: install an update found there at once\n"
           L"Used by the updater:\n"
           L"  --after-update <pid>      the updated window waits for the old one to close\n";
}

}  // namespace st::ui
