// File-based automation channel for hidden test runs (docs/hidden-testing.md).
//
// Off unless <game>\stranded_together\session.ini says automation=1 (the
// tools/testenv supervisor writes that; the launcher never does). Then:
//
//   <mod>\automation\in\*.cmd    commands, one per line, files taken in name order
//   <mod>\automation\out\        ready.txt, status.txt (1 Hz), results.txt (one line
//                                per command), <file>.done per command file, PNG
//                                screenshots
//
// Commands (case-insensitive keyword, arguments separated by blanks):
//   wait <ms>                      sleep on the automation thread
//   log <text>                     one "[auto] <text>" line in stranded_together.log
//   screenshot <name>              the game's back buffer as out\<name>.png (menu open or closed)
//   pad <buttons> <ms> [lx= ly= rx= ry= lt= rt=]
//                                  virtual XInput pad 0: hold buttons (A+START, DPAD_DOWN,
//                                  none, ...) and sticks for ms, then idle (still connected)
//   padoff                         virtual pad disconnected again
//   key <name|0xVK> <ms>           WM_KEYDOWN/WM_KEYUP posted to the game window (window
//                                  procedure only: posted keys never reach WH_KEYBOARD hooks)
//   menu open|close|toggle         the debug menu
//   action <name> [numbers...]     a debug action (st::debug registry, label with spaces as
//                                  underscores: respawn_partner, log_status, teleport x y z).
//                                  Numbers fill Request::v[0..2] and f[0..1]; the first one
//                                  also sets flag (!= 0) and, when it is the only one, i
//   waitplayer <s>                 until the tick's snapshot has the local player
//   waitlog <s> <text>             until a NEW log line contains <text>
//   snapshot                       the tick's snapshot as one result line
//   quit                           WM_CLOSE to the game window, process ends after 20 s at most
//
// The thread only does file I/O, queues debug actions (they run on the game
// thread, st::debug::OnTick), asks the overlay for captures (taken inside
// Present) and sets the virtual pad state (answered by the XInputGetState hook).
// It never reads or calls game memory.
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace st::automation {

// Bootstrap thread, BEFORE st::LoadConfig consumes session.ini.
// Reads automation=1 / automation_mute from <mod_dir>\session.ini.
bool Init(const std::wstring& mod_dir) noexcept;
bool Enabled() noexcept;
bool MuteRequested() noexcept;
// automation_isolate (default 1): the overlay's automation hooks (virtual pad only,
// GetForegroundWindow, cursor isolation, no exclusive fullscreen, no occlusion).
// 0 keeps the command channel and screenshots without them (bisecting a start-up problem).
bool IsolateRequested() noexcept;
// Bootstrap thread, after the overlay was installed (or not): starts the
// command thread. No-op unless Init found automation=1.
void Start(const std::wstring& mod_dir, const char* build) noexcept;

// ------------------------------------------------------------- parsing
// Pure functions (header-only so tests/testenv can check them without the mod).
enum class Kind : std::uint8_t {
    unknown, wait, log, screenshot, pad, padoff, key, menu, action, waitplayer, waitlog, snapshot, quit
};

// XInput button bits (same values as XINPUT_GAMEPAD_*).
constexpr std::uint16_t kPadUp = 0x0001, kPadDown = 0x0002, kPadLeft = 0x0004, kPadRight = 0x0008,
                        kPadStart = 0x0010, kPadBack = 0x0020, kPadLS = 0x0040, kPadRS = 0x0080,
                        kPadLB = 0x0100, kPadRB = 0x0200, kPadA = 0x1000, kPadB = 0x2000, kPadX = 0x4000,
                        kPadY = 0x8000;

struct Command {
    Kind kind = Kind::unknown;
    std::string text;              // log text, screenshot name, action name, waitlog needle, menu state
    std::vector<double> numbers;   // action arguments
    std::uint32_t ms = 0;          // wait / hold time / timeout
    std::uint16_t buttons = 0;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::uint8_t lt = 0, rt = 0;
    std::uint32_t vk = 0;
};

namespace detail {
inline std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
inline std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        const std::size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}
inline bool Number(const std::string& s, double lo, double hi, double* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!end || *end != 0 || !(v >= lo && v <= hi)) return false;
    *out = v;
    return true;
}
inline bool Uint(const std::string& s, std::uint32_t lo, std::uint32_t hi, std::uint32_t* out) {
    if (s.empty() || s[0] == '-' || s[0] == '+') return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (!end || *end != 0 || v < lo || v > hi) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}
// Text after the first `skip` blank-separated words (keeps inner spaces).
inline std::string Rest(const std::string& line, int skip) {
    std::size_t i = 0;
    for (int w = 0; w < skip; ++w) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    }
    return Trim(line.substr(i < line.size() ? i : line.size()));
}
}  // namespace detail

// "A+START", "dpad_down", "none" -> XInput bits. False on an unknown name.
inline bool ParsePadButtons(const std::string& spec, std::uint16_t* out) {
    struct Name {
        const char* name;
        std::uint16_t bit;
    };
    static const Name kNames[] = {
        {"a", kPadA}, {"b", kPadB}, {"x", kPadX}, {"y", kPadY}, {"start", kPadStart}, {"menu", kPadStart},
        {"back", kPadBack}, {"view", kPadBack}, {"select", kPadBack}, {"lb", kPadLB}, {"rb", kPadRB},
        {"ls", kPadLS}, {"rs", kPadRS}, {"up", kPadUp}, {"down", kPadDown}, {"left", kPadLeft},
        {"right", kPadRight}, {"dpad_up", kPadUp}, {"dpad_down", kPadDown}, {"dpad_left", kPadLeft},
        {"dpad_right", kPadRight}};
    const std::string s = detail::Lower(spec);
    if (s == "none" || s == "0") {
        *out = 0;
        return true;
    }
    std::uint16_t bits = 0;
    std::size_t i = 0;
    while (i <= s.size()) {
        std::size_t j = s.find('+', i);
        if (j == std::string::npos) j = s.size();
        const std::string part = s.substr(i, j - i);
        bool found = false;
        for (const Name& n : kNames)
            if (part == n.name) bits = static_cast<std::uint16_t>(bits | n.bit), found = true;
        if (!found) return false;
        i = j + 1;
    }
    *out = bits;
    return true;
}

// F1..F12, ESC, ENTER, SPACE, TAB, BACKSPACE, UP/DOWN/LEFT/RIGHT, a letter or digit, 0xNN.
inline bool ParseKeyName(const std::string& spec, std::uint32_t* vk) {
    const std::string s = detail::Lower(spec);
    if (s.size() == 1 && std::isalnum(static_cast<unsigned char>(s[0]))) {
        *vk = static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(s[0])));
        return true;
    }
    if ((s.size() == 2 || s.size() == 3) && s[0] == 'f') {
        std::uint32_t n = 0;
        if (detail::Uint(s.substr(1), 1, 12, &n)) {
            *vk = 0x70 + n - 1;
            return true;
        }
        return false;
    }
    struct Name {
        const char* name;
        std::uint32_t vk;
    };
    static const Name kNames[] = {{"esc", 0x1B},   {"escape", 0x1B}, {"enter", 0x0D}, {"return", 0x0D},
                                  {"space", 0x20}, {"tab", 0x09},    {"backspace", 0x08}, {"up", 0x26},
                                  {"down", 0x28},  {"left", 0x25},   {"right", 0x27}};
    for (const Name& n : kNames)
        if (s == n.name) {
            *vk = n.vk;
            return true;
        }
    return s.rfind("0x", 0) == 0 && detail::Uint(s, 1, 0xFE, vk);
}

// [A-Za-z0-9_.-], 1..64 chars, not starting with '.'.
inline bool ValidShotName(const std::string& name) {
    if (name.empty() || name.size() > 64 || name[0] == '.') return false;
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

// One command line. Blank lines and '#' comments give Kind::unknown with an empty error.
inline bool ParseCommand(const std::string& raw, Command* out, std::string* error) {
    *out = Command{};
    error->clear();
    const std::string line = detail::Trim(raw);
    if (line.empty() || line[0] == '#') return false;
    const std::vector<std::string> w = detail::Split(line);
    const std::string verb = detail::Lower(w[0]);
    const auto need = [&](std::size_t n, const char* usage) {
        if (w.size() >= n) return true;
        *error = std::string("usage: ") + usage;
        return false;
    };
    constexpr std::uint32_t kMaxMs = 600000;
    if (verb == "wait") {
        if (!need(2, "wait <ms>")) return false;
        if (!detail::Uint(w[1], 0, kMaxMs, &out->ms)) return *error = "wait: 0..600000 ms", false;
        out->kind = Kind::wait;
    } else if (verb == "log") {
        out->kind = Kind::log;
        out->text = detail::Rest(line, 1);
    } else if (verb == "screenshot") {
        if (!need(2, "screenshot <name>")) return false;
        if (!ValidShotName(w[1])) return *error = "screenshot: name must be [A-Za-z0-9_.-], 1..64 chars", false;
        out->kind = Kind::screenshot;
        out->text = w[1];
    } else if (verb == "pad") {
        if (!need(3, "pad <buttons> <ms> [lx= ly= rx= ry= lt= rt=]")) return false;
        if (!ParsePadButtons(w[1], &out->buttons)) return *error = "pad: unknown button in '" + w[1] + "'", false;
        if (!detail::Uint(w[2], 0, kMaxMs, &out->ms)) return *error = "pad: hold time 0..600000 ms", false;
        for (std::size_t i = 3; i < w.size(); ++i) {
            const std::string kv = detail::Lower(w[i]);
            const auto eq = kv.find('=');
            double v = 0;
            const std::string k = eq == std::string::npos ? kv : kv.substr(0, eq);
            const std::string val = eq == std::string::npos ? std::string() : kv.substr(eq + 1);
            const bool trigger = k == "lt" || k == "rt";
            if (!detail::Number(val, trigger ? 0 : -32768, trigger ? 255 : 32767, &v))
                return *error = "pad: bad axis '" + w[i] + "' (lx/ly/rx/ry -32768..32767, lt/rt 0..255)", false;
            if (k == "lx") out->lx = static_cast<std::int16_t>(v);
            else if (k == "ly") out->ly = static_cast<std::int16_t>(v);
            else if (k == "rx") out->rx = static_cast<std::int16_t>(v);
            else if (k == "ry") out->ry = static_cast<std::int16_t>(v);
            else if (k == "lt") out->lt = static_cast<std::uint8_t>(v);
            else if (k == "rt") out->rt = static_cast<std::uint8_t>(v);
            else return *error = "pad: unknown axis '" + k + "'", false;
        }
        out->kind = Kind::pad;
    } else if (verb == "padoff") {
        out->kind = Kind::padoff;
    } else if (verb == "key") {
        if (!need(3, "key <name|0xVK> <ms>")) return false;
        if (!ParseKeyName(w[1], &out->vk)) return *error = "key: unknown key '" + w[1] + "'", false;
        if (!detail::Uint(w[2], 0, 60000, &out->ms)) return *error = "key: hold time 0..60000 ms", false;
        out->kind = Kind::key;
    } else if (verb == "menu") {
        if (!need(2, "menu open|close|toggle")) return false;
        out->text = detail::Lower(w[1]);
        if (out->text != "open" && out->text != "close" && out->text != "toggle") return *error = "menu: open|close|toggle", false;
        out->kind = Kind::menu;
    } else if (verb == "action") {
        if (!need(2, "action <name> [numbers...]")) return false;
        out->text = detail::Lower(w[1]);
        for (std::size_t i = 2; i < w.size(); ++i) {
            double v = 0;
            if (!detail::Number(w[i], -1e9, 1e9, &v)) return *error = "action: argument '" + w[i] + "' is not a number", false;
            out->numbers.push_back(v);
        }
        if (out->numbers.size() > 3) return *error = "action: at most 3 numbers", false;
        out->kind = Kind::action;
    } else if (verb == "waitplayer") {
        std::uint32_t s = 0;
        if (!need(2, "waitplayer <seconds>")) return false;
        if (!detail::Uint(w[1], 1, 600, &s)) return *error = "waitplayer: 1..600 s", false;
        out->ms = s * 1000;
        out->kind = Kind::waitplayer;
    } else if (verb == "waitlog") {
        std::uint32_t s = 0;
        if (!need(3, "waitlog <seconds> <text>")) return false;
        if (!detail::Uint(w[1], 1, 600, &s)) return *error = "waitlog: 1..600 s", false;
        out->ms = s * 1000;
        out->text = detail::Rest(line, 2);
        out->kind = Kind::waitlog;
    } else if (verb == "snapshot") {
        out->kind = Kind::snapshot;
    } else if (verb == "quit") {
        out->kind = Kind::quit;
    } else {
        *error = "unknown command '" + w[0] + "'";
        return false;
    }
    return true;
}

// "Respawn partner" / "respawn_partner" / "RESPAWN-PARTNER" -> "respawn_partner".
inline std::string NormalizeActionName(const std::string& s) {
    std::string o;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) o += static_cast<char>(std::tolower(u));
        else if ((c == ' ' || c == '_' || c == '-') && !o.empty() && o.back() != '_') o += '_';
    }
    while (!o.empty() && o.back() == '_') o.pop_back();
    return o;
}

}  // namespace st::automation
