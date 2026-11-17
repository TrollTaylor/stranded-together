// Sandbox saves and local two-instance tests (docs/sandbox-and-local-test.md).
//
// The save redirect and the foreign-save bypass of the hidden test runs (automation.hpp
// "save redirect" / "foreign saves") also run in normal, visible play when the launcher's
// one-shot session.ini asks for them:
//
//   sandbox_saves=<name>        every access to %LOCALAPPDATA%\KojimaProductions goes to
//                               <game>\stranded_together\sandbox\<name>\saves (seeded by the
//                               launcher; it must hold the redirect probe file). The user's
//                               real save tree is never opened by the game.
//   sandbox_foreign_saves=1     also skip ds.exe's save-owner check (a save made by another
//                               account, e.g. a downloaded 100% save); only after the redirect
//                               self-test passed.
//   appdir_settings=1           <game>\settings.cfg (ds.exe's "appdir:settings.cfg": the graphics
//                               settings) is redirected too, to sandbox\<name>\settings.cfg
//                               (automation: automation\settings.cfg). The launcher's local test
//                               writes low-quality windowed settings there.
//   second_instance=1           a second ds.exe on this PC: its single-instance mutex
//                               (DeathStrandingDCAppMutex, CreateMutexA at 0x1417EAE1D; exit(1)
//                               on ERROR_ALREADY_EXISTS at 0x1417EAE96) is renamed for this
//                               process only. Only with an active redirect (two games must never
//                               share one save tree).
//   local_input=host|host_pad|guest   input split of the local two-instance test (overlay_input).
//
// Without these keys nothing here runs. When a requested redirect cannot be installed the
// mod ends ds.exe in DllMain (exit code kRefusedExitCode) and leaves
// <mod>\sandbox-refused.txt with the reason; the launcher shows it.
//
// Per-instance mod folder: ST_INSTANCE_DIR (environment of the process, set only by the
// launcher's local test and the testenv supervisor) names a folder below
// <game>\stranded_together (e.g. "instances\guest"); session.ini, the log, link-status.json
// and the automation channel of that instance live there. A game started with ST_INSTANCE_DIR
// but without a redirected session.ini (one-shot file gone, ds.exe restarted) is refused, never
// run on the real saves (Mode::refuse_instance).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace st::sandbox {

constexpr unsigned long kRefusedExitCode = 0x53540006ul;            // "ST" 0006
constexpr const wchar_t* kRefusedFile = L"sandbox-refused.txt";     // in the instance's mod folder
constexpr const wchar_t* kActiveFile = L"sandbox-active.txt";       // in the instance's mod folder
constexpr const wchar_t* kInstanceEnv = L"ST_INSTANCE_DIR";
constexpr const char* kAppMutex = "DeathStrandingDCAppMutex";
constexpr const char* kMutexSuffix = "-StrandedTogether-2";

// [A-Za-z0-9_-], 1..40 characters: a folder name under stranded_together\sandbox.
inline bool ValidSandboxName(const std::string& s) {
    if (s.empty() || s.size() > 40) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// ST_INSTANCE_DIR: 1..3 components of [A-Za-z0-9_-] (1..32 each) separated by '\' or '/',
// relative, no "." or "..": always a folder below stranded_together. *out = '\'-separated.
inline bool ValidInstanceDir(const std::wstring& s, std::wstring* out = nullptr) {
    if (s.empty() || s.size() > 100) return false;
    std::wstring norm;
    int comps = 0, len = 0;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'/') {
            if (len == 0) return false;  // leading or doubled separator
            norm += L'\\';
            len = 0;
            continue;
        }
        const bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-';
        if (!ok) return false;
        if (len == 0) ++comps;
        if (++len > 32) return false;
        norm += c;
    }
    if (len == 0 || comps < 1 || comps > 3) return false;
    if (out) *out = norm;
    return true;
}

// <exe dir>\stranded_together, or <exe dir>\stranded_together\<ST_INSTANCE_DIR> when `instance`
// is valid. `instance` empty or invalid: the default folder (*used_instance = false).
inline std::wstring ModDirFor(const std::wstring& exe_dir, const std::wstring& instance, bool* used_instance = nullptr) {
    std::wstring rel;
    const bool use = !instance.empty() && ValidInstanceDir(instance, &rel);
    if (used_instance) *used_instance = use;
    return exe_dir + L"\\stranded_together" + (use ? L"\\" + rel : std::wstring());
}

// The second instance's name for ds.exe's single-instance mutex: "" when `name` is not that
// mutex (exact name, optionally with a Local\ or Global\ namespace prefix, which is kept).
inline std::string RenamedMutex(const char* name) {
    if (!name) return {};
    std::string n(name, strnlen(name, 512));
    std::string prefix;
    for (const char* p : {"Local\\", "Global\\"})
        if (n.rfind(p, 0) == 0) {
            prefix = p;
            n = n.substr(std::strlen(p));
            break;
        }
    if (n != kAppMutex) return {};
    return prefix + n + kMutexSuffix;
}
inline std::wstring RenamedMutexW(const wchar_t* name) {
    if (!name) return {};
    std::string narrow;
    for (const wchar_t* p = name; *p && narrow.size() < 512; ++p) {
        if (*p > 0x7E || *p < 0x20) return {};
        narrow += static_cast<char>(*p);
    }
    const std::string r = RenamedMutex(narrow.c_str());
    return std::wstring(r.begin(), r.end());
}

enum class LocalInput : std::uint8_t {
    off,       // a normal game
    host,      // local two-instance test, first instance: keyboard + mouse, XInput pads hidden
    host_pad,  // the same, but XInput pads too (the user wants a pad on the host)
    guest,     // second instance: XInput pads; never grabs the cursor; keeps running unfocused
};
inline LocalInput ParseLocalInput(const std::string& v) {
    if (v == "host") return LocalInput::host;
    if (v == "host_pad") return LocalInput::host_pad;
    if (v == "guest") return LocalInput::guest;
    return LocalInput::off;
}
inline const char* LocalInputName(LocalInput m) {
    switch (m) {
        case LocalInput::host: return "host";
        case LocalInput::host_pad: return "host_pad";
        case LocalInput::guest: return "guest";
        default: return "off";
    }
}

// What DllMain does with a session.ini (pure; automation.cpp acts on it).
struct Request {
    bool automation = false;          // automation=1 (hidden runs; its own rules in automation.cpp)
    std::string sandbox;              // sandbox_saves=<name>
    bool sandbox_foreign = false;     // sandbox_foreign_saves=1
    bool appdir_settings = false;     // appdir_settings=1
    bool second_instance = false;     // second_instance=1
    // ST_INSTANCE_DIR is set in the environment (any value): only the launcher's local test and
    // the testenv supervisor start such a game, always with a redirected session.ini. Without one
    // (a one-shot file already consumed, deleted, or ds.exe restarted itself) it must not run on
    // the real saves.
    bool instance_env = false;
};
enum class Mode : std::uint8_t {
    none,              // nothing requested: no hooks, no change (every normal launch)
    automation,        // a hidden run (automation.cpp's checks)
    sandbox,           // visible play on a sandbox copy
    refuse_bad_name,   // sandbox_saves is not a valid name: refuse (never guess a folder)
    refuse_orphan,     // second_instance / appdir_settings / sandbox_foreign_saves without any redirect: refuse
    refuse_instance,   // ST_INSTANCE_DIR set but no redirect requested: refuse (a local-test game never uses the real saves)
};
inline Mode Decide(const Request& r) {
    if (r.automation) return Mode::automation;
    if (!r.sandbox.empty()) return ValidSandboxName(r.sandbox) ? Mode::sandbox : Mode::refuse_bad_name;
    if (r.second_instance || r.appdir_settings || r.sandbox_foreign) return Mode::refuse_orphan;
    if (r.instance_env) return Mode::refuse_instance;
    return Mode::none;
}
// The foreign-save bypass in sandbox mode: requested and the redirect installed + self-tested.
inline bool SandboxForeignAllowed(bool requested, bool redirect_active) { return requested && redirect_active; }

// Graphics for two games on one PC: the text of ds.exe's "appdir:settings.cfg" (the game folder's
// settings.cfg; read by FUN_1435867B0 and parsed by FUN_141A02D80 like game_settings.cfg: one
// `"key" "value"` line each). The keys are the ones the engine writes there itself
// (FUN_142644FF0, read back by FUN_142643B40): rendering_width/height, window_left/top/right/
// bottom, window_mode, wait_vsync, postprocess_dof/motionblur/ssr/ao, streaming_memory_mb,
// lod_scale, limit_fps, max_shadowmap_size (0: 512, 1: 1024, 2: larger), anti-aliasing, the
// upscalers; "fps" is read at boot (FUN_14262E480, clamped 15..240). Written by the launcher's local
// test into each sandbox (appdir_settings=1 redirects the game folder's file there, so the user's
// own one is never read or written) and by the testenv's "graphics low".
// window_mode is a windowed switch: the game writes it as ([display+0x62] == 0) ? 1 : 0
// (0x1426454D4..0x142645521), so 0 = fullscreen, the default when the key is absent. Earlier
// versions of this text had no window_mode: on a player's desktop the local test's host went
// fullscreen at 1920x1080 (its own rewrite of the file: window_mode 0, window 0,0-1920,1080) and
// the guest's renderer set-up then failed ("Error initializing rendering configuration"), every
// time; hidden runs on a private desktop never showed it. So: windowed, 1280x720, and the window
// rect (the overlay then moves each window to its half: local_window).
// The game rewrites this file on exit with its own values, so the launcher merges these keys into
// it before every local test (MergeGameSettings), not only when the file is missing.
// The file must be COMPLETE: with keys missing, ds.exe's boot check (0x14262E9D1..0x14262E9F9)
// shows "Invalid Graphics Configration. Set Graphics Setting Default" (a 10 s message box), resets
// every graphics setting to its defaults (FUN_142644660: fullscreen again) and, next to a running
// first game, the second game's renderer set-up then failed (v0.5.3 on a player's desktop: the
// host, whose file the game had completed on an earlier exit, ran windowed; the guest did not).
// All keys below, in the game's own order, with the values the game itself wrote for this preset.
inline const char* LowGraphicsSettingsCfg() {
    return "\"rendering_width\" \"1280\"\r\n"
           "\"rendering_height\" \"720\"\r\n"
           "\"wait_vsync\" \"1\"\r\n"
           "\"fps\" \"30\"\r\n"
           "\"limit_fps\" \"30\"\r\n"
           "\"postprocess_dof\" \"0\"\r\n"
           "\"postprocess_motionblur\" \"0\"\r\n"
           "\"postprocess_ssr\" \"0\"\r\n"
           "\"postprocess_ao\" \"0\"\r\n"
           "\"max_shadowmap_size\" \"0\"\r\n"
           "\"hdr\" \"0\"\r\n"
           "\"window_left\" \"0\"\r\n"
           "\"window_top\" \"0\"\r\n"
           "\"window_right\" \"1280\"\r\n"
           "\"window_bottom\" \"720\"\r\n"
           "\"window_mode\" \"1\"\r\n"
           "\"streaming_memory_mb\" \"2816\"\r\n"
           "\"lod_scale\" \"2\"\r\n"
           "\"anti-aliasing\" \"2\"\r\n"
           "\"amd_fsr\" \"0\"\r\n"
           "\"amd_fsr_sharpness\" \"87\"\r\n"
           "\"amd_fsr_mode\" \"0\"\r\n"
           "\"amd_fsr2\" \"0\"\r\n"
           "\"amd_fsr2_mode\" \"1\"\r\n"
           "\"amd_fsr2_sharpness_value\" \"80\"\r\n"
           "\"nvidia_nis\" \"0\"\r\n"
           "\"nvidia_nis_sharpness\" \"10\"\r\n"
           "\"nvidia_nis_mode\" \"0\"\r\n"
           "\"nvidia_dlss\" \"0\"\r\n"
           "\"nvidia_dlss_sharpness\" \"50\"\r\n"
           "\"nvidia_dlss_quality\" \"0\"\r\n"
           "\"intel_xess\" \"0\"\r\n"
           "\"intel_xess_mode\" \"1\"\r\n";
}

// The graphics menu's options in the profile's game_settings.cfg (keys from the settings table
// next to 0x143DF5990: WindowMode, Resolution, GraphicQuality, ModelDetail, ...; a key the user
// never changed is absent). Only ever merged into a sandbox/scratch copy of the profile (testenv
// "graphics low"; the launcher does not write it). Values unverified, see above.
inline const char* LowGraphicsGameSettings() {
    return "\"GraphicQuality\" \"0\"\r\n"
           "\"VerticalSync\" \"1\"\r\n";
}

// `original` with every `"Key" "Value"` line of `overrides` replacing the line of the same key
// (or appended). Line ends of the original are kept (CRLF when it has them). Pure.
inline std::string MergeGameSettings(const std::string& original, const std::string& overrides) {
    const auto key_of = [](const std::string& line) -> std::string {
        const auto a = line.find('"');
        if (a == std::string::npos) return {};
        const auto b = line.find('"', a + 1);
        return b == std::string::npos ? std::string() : line.substr(a + 1, b - a - 1);
    };
    const auto split = [](const std::string& text) {
        std::vector<std::string> lines;
        std::size_t at = 0;
        while (at < text.size()) {
            std::size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string l = text.substr(at, nl - at);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            at = nl + 1;
        }
        return lines;
    };
    const std::string eol = original.find("\r\n") != std::string::npos || original.empty() ? "\r\n" : "\n";
    std::vector<std::string> lines = split(original);
    for (const std::string& o : split(overrides)) {
        const std::string k = key_of(o);
        if (k.empty()) continue;
        bool done = false;
        for (std::string& l : lines)
            if (key_of(l) == k) l = o, done = true;
        if (!done) lines.push_back(o);
    }
    std::string out;
    for (const std::string& l : lines)
        if (!l.empty()) out += l + eol;
    return out;
}

// ---- runtime (sandbox.cpp) ------------------------------------------------------------
// DllMain: hooks CreateMutexA/W and CreateMutexExA/W (kernelbase) so that this process
// creates kAppMutex + kMutexSuffix instead of kAppMutex. MinHook must be initialised.
// False (and *why) when a required hook could not be installed.
bool InstallMutexRename(char* why, std::size_t cap) noexcept;
// How often the rename happened (0 until ds.exe's static initializer ran).
unsigned long MutexRenames() noexcept;
bool MutexRenameInstalled() noexcept;

}  // namespace st::sandbox
