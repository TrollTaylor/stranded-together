// Stranded Together launcher core: install gate, Steam account, settings and
// Collect logs (see core_install.hpp).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core_install.hpp"

#include "core_text.hpp"
#include "launcher/zip.hpp"

namespace st::core {
namespace {

bool ReadJson(const std::wstring& path, json::Value& out, std::uint64_t max = 4u << 20) {
    std::string text;
    if (!IsFile(path) || !ReadTextShared(path, text, max)) return false;
    return json::Parse(text, out);
}

std::int64_t WriteTimeMs(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    const std::uint64_t ticks = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                data.ftLastWriteTime.dwLowDateTime;
    return static_cast<std::int64_t>(ticks / 10000ull) - 11644473600000ll;
}

// The SHA-256 of `path`, from settings[key] when its size and write time are unchanged.
bool CachedHash(const std::wstring& path, json::Value& settings, const char* key, std::string& hex) {
    const std::uint64_t size = FileSizeOrZero(path);
    const std::int64_t written = WriteTimeMs(path);
    if (const json::Value* c = settings.Find(key)) {
        if (c->is_object() && static_cast<std::uint64_t>(c->Int("size", -1)) == size && c->Int("write_ms", -1) == written &&
            IsHexText(c->Str("sha256"), 64)) {
            hex = c->Str("sha256");
            return true;
        }
    }
    if (!Sha256FileHex(path, hex)) return false;
    json::Value entry = json::Value::Object();
    entry["size"] = size;
    entry["write_ms"] = written;
    entry["sha256"] = hex;
    settings[key] = entry;
    return true;
}

}  // namespace

GateResult CheckGate(Env&, const InstancePaths& paths, json::Value& settings) {
    GateResult g;
    g.game_found = IsFile(paths.exe);
    if (!g.game_found) return g;
    g.path_problem = GamePathProblem(paths.game);
    std::string hex;
    if (CachedHash(paths.exe, settings, "exe_cache", hex)) {
        g.exe_hashed = true;
        g.exe_known = hex == kExeSha256;
    }
    if (IsFile(paths.steam_api) && CachedHash(paths.steam_api, settings, "steam_api_cache", hex)) g.steam_api_known = hex == kSteamApiSha256;
    g.mod_present = IsFile(paths.mod_dll) && !IsReparsePoint(paths.mod_dll);
    return g;
}

std::string ModMissingText() {
    return "The co-op mod is not installed in this game folder. Extract everything from the Stranded Together zip into the "
           "game folder, so winhttp.dll sits next to ds.exe, then press Check again.";
}

std::string UntestedVersionText(const GateResult& gate) {
    if (!gate.game_found) return {};
    if (!gate.exe_hashed)
        return "ds.exe could not be read to check its version. Co-op may still work; if the game misbehaves, verify the game "
               "files in Steam.";
    if (!gate.exe_known)
        return "This ds.exe is not the build Stranded Together was made for (Steam may have updated the game). You can still "
               "play, but co-op may misbehave or crash until the mod is updated.";
    if (!gate.steam_api_known)
        return "steam_api64.dll is not the one Stranded Together was tested with. You can still play, but Steam co-op may not "
               "work; LAN mode is unaffected.";
    return {};
}

// ---- package.json and winhttp.dll ----------------------------------------------------
PackageInfo ParsePackageText(const std::string& text) {
    PackageInfo p;
    p.present = true;
    const std::string damaged = "stranded_together\\package.json is damaged. Extract the whole Stranded Together zip into the game folder again.";
    json::Value v;
    if (!json::Parse(text, v) || !v.is_object() || v.Str("product") != kPackageProduct || v.Int("v", 0) != 1) {
        p.problem = damaged;
        return p;
    }
    p.version = v.Str("version");
    p.build_id = v.Str("build_id");
    if (const json::Value* c = v.Find("source_commit"); c && c->is_string()) p.source_commit = c->StringRef();
    int dlls = 0;
    if (const json::Value* files = v.Find("files"); files && files->is_array()) {
        for (const json::Value& row : files->Elements()) {
            if (!row.is_object()) continue;
            const std::string path = LowerAscii(row.Str("path"));
            const std::string sha = UpperAscii(row.Str("sha256"));
            if (path == "winhttp.dll") {
                ++dlls;
                p.dll_sha256 = sha;
            } else if (path == "strandedtogether.exe") {
                p.exe_sha256 = sha;
            }
        }
    }
    if (p.version.empty() || dlls != 1 || !IsHexText(p.dll_sha256, 64) || (!p.exe_sha256.empty() && !IsHexText(p.exe_sha256, 64))) {
        p.problem = damaged;
        p.dll_sha256.clear();
        p.exe_sha256.clear();
        return p;
    }
    if (p.build_id.empty()) p.build_id = p.version;
    p.ok = true;
    return p;
}

PackageInfo ReadPackage(const InstancePaths& paths) {
    PackageInfo p;
    if (!IsFile(paths.package_json)) return p;
    std::string text;
    if (IsReparsePoint(paths.package_json) || !ReadTextShared(paths.package_json, text, 1u << 20)) {
        p.present = true;
        p.problem = "stranded_together\\package.json could not be read. Extract the whole Stranded Together zip into the game folder again.";
        return p;
    }
    return ParsePackageText(text);
}

bool LooksLikeOurDllBytes(const std::string& bytes) {
    if (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z') return false;
    // The forwarders the proxy generator writes (tools/gen_winhttp_proxy.py).
    static const char kForwarder[] = "\\\\.\\GLOBALROOT\\SystemRoot\\System32\\winhttp.dll.";
    if (bytes.find(kForwarder) == std::string::npos) return false;
    static const char kFolder[] = "stranded_together";
    if (bytes.find(kFolder) != std::string::npos) return true;
    std::string wide;
    for (const char* c = kFolder; *c; ++c) {
        wide.push_back(*c);
        wide.push_back('\0');
    }
    return bytes.find(wide) != std::string::npos;
}

bool LooksLikeOurDll(const std::wstring& path) {
    Bytes data;
    std::string error;
    if (!IsFile(path) || IsReparsePoint(path) || !ReadFileShared(path, data, 64u << 20, error)) return false;
    return LooksLikeOurDllBytes(std::string(data.begin(), data.end()));
}

ModCheck CheckMod(const InstancePaths& paths, const PackageInfo& package, const std::string& launcher_version) {
    ModCheck m;
    if (!IsFile(paths.mod_dll)) {
        std::string hex;
        if (IsFile(paths.mod_dll_disabled) && !IsReparsePoint(paths.mod_dll_disabled) &&
            ((package.ok && Sha256FileHex(paths.mod_dll_disabled, hex) && hex == package.dll_sha256) || LooksLikeOurDll(paths.mod_dll_disabled))) {
            m.state = ModState::disabled;
            m.note = ModDisabledText();
        }
        return m;
    }
    if (IsReparsePoint(paths.mod_dll)) {
        m.state = ModState::foreign;
        m.note = "winhttp.dll in the game folder is a link (junction or symbolic link), not the Stranded Together file. "
                 "Replace it with the one from the zip.";
        return m;
    }
    if (!Sha256FileHex(paths.mod_dll, m.dll_sha256)) {
        m.state = ModState::foreign;
        m.note = "winhttp.dll in the game folder could not be read. Close programs that may hold it, then press Check again.";
        return m;
    }
    const bool hash_ok = package.ok && m.dll_sha256 == package.dll_sha256;
    if (!hash_ok && !LooksLikeOurDll(paths.mod_dll)) {
        m.state = ModState::foreign;
        m.note = ModForeignText();
        return m;
    }
    if (!package.present) {
        m.state = ModState::dev_build;
        m.note = "Development build: there is no stranded_together\\package.json, so winhttp.dll is not checked against a release.";
        return m;
    }
    if (!package.ok) {
        m.state = ModState::mismatch;
        m.note = package.problem;
        return m;
    }
    if (!hash_ok) {
        m.state = ModState::mismatch;
        m.note = "winhttp.dll is not the one from this Stranded Together package (" + package.version +
                 "). Extract the whole zip into the game folder again; both players need the same version.";
        return m;
    }
    std::string exe_hex;
    if (!package.exe_sha256.empty() && IsFile(paths.launcher_exe) && Sha256FileHex(paths.launcher_exe, exe_hex) &&
        exe_hex != package.exe_sha256) {
        m.state = ModState::mismatch;
        m.note = "StrandedTogether.exe is not the one from this package (" + package.version +
                 "). Extract the whole zip into the game folder again.";
        return m;
    }
    if (!launcher_version.empty() && package.build_id != launcher_version) {
        m.state = ModState::mismatch;
        m.note = "The files in this folder come from different Stranded Together builds (the package is " + package.build_id +
                 ", this launcher is " + launcher_version + "). Extract the whole zip into the game folder again.";
        return m;
    }
    m.state = ModState::ok;
    return m;
}

std::string ModForeignText() {
    return "There is a winhttp.dll in the game folder that is not Stranded Together (another mod or tool uses the same file name). "
           "Stranded Together never replaces it. Move it out of the game folder yourself, extract the Stranded Together zip again, "
           "then press Check again.";
}

std::string ModDisabledText() {
    return "The co-op mod is switched off (winhttp.dll.disabled), so the game runs unmodded. Press Enable mod to switch it back on.";
}

bool SetModEnabled(Env& env, const InstancePaths& paths, const PackageInfo& package, bool enable, std::string& error) {
    if (AnyGameRunningIn(env, paths.game)) {
        error = "DEATH STRANDING is running. Quit it first; Windows keeps winhttp.dll in use while the game runs.";
        return false;
    }
    const std::wstring& from = enable ? paths.mod_dll_disabled : paths.mod_dll;
    const std::wstring& to = enable ? paths.mod_dll : paths.mod_dll_disabled;
    if (!IsFile(from)) {
        error = enable ? "There is no winhttp.dll.disabled to switch back on." : "There is no winhttp.dll to switch off.";
        return false;
    }
    std::string hex;
    const bool ours = !IsReparsePoint(from) &&
                      ((package.ok && Sha256FileHex(from, hex) && hex == package.dll_sha256) || LooksLikeOurDll(from));
    if (!ours) {
        error = enable ? "winhttp.dll.disabled is not the Stranded Together file, so it is left alone."
                       : "winhttp.dll is not the Stranded Together file (another mod or tool), so it is left alone.";
        return false;
    }
    if (PathExists(to)) {
        if (enable) {
            error = "There already is a winhttp.dll in the game folder. Move it away first; Stranded Together never overwrites it.";
            return false;
        }
        // An older winhttp.dll.disabled of ours makes way for the current one.
        if (!LooksLikeOurDll(to) || !DeleteFileChecked(to)) {
            error = "winhttp.dll.disabled already exists and could not be replaced.";
            return false;
        }
    }
    if (!MovePath(from, to, error)) {
        error = "Could not rename " + Utf8(LeafOf(from)) + ": " + error;
        return false;
    }
    return true;
}

// ---- Steam account ---------------------------------------------------------------------
std::uint32_t SteamActiveUser(Env& env) {
    std::uint32_t v = 0;
    return env.RegDword(false, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", v) ? v : 0;
}

std::uint64_t OwnSteamId(Env& env) {
    const std::uint32_t a = SteamActiveUser(env);
    return a ? kSteamIdBase + a : 0;
}

bool SteamClientRunning(Env& env) {
    std::uint32_t pid = 0;
    if (!env.RegDword(false, L"Software\\Valve\\Steam\\ActiveProcess", L"pid", pid) || pid == 0) return false;
    return env.ProcessAlive(pid) && SteamActiveUser(env) != 0;
}

bool SteamAppRunningFlag(Env& env) {
    std::uint32_t running = 0, app = 0;
    const bool have_running = env.RegDword(false, L"Software\\Valve\\Steam\\Apps\\1850570", L"Running", running);
    const bool have_app = env.RegDword(false, L"Software\\Valve\\Steam", L"RunningAppID", app);
    return (have_running && running != 0) || (have_app && app == kSteamAppId);
}

// ---- launcher-settings.json -----------------------------------------------------------
bool ReadSettings(const InstancePaths& paths, json::Value& out) {
    json::Value v;
    if (ReadJson(paths.settings, v) && v.is_object() && v.Str("product") == "stranded-together-launcher-settings") {
        out = std::move(v);
        return true;
    }
    out = json::Value::Object();
    out["product"] = "stranded-together-launcher-settings";
    out["v"] = 1;
    return false;
}

bool SaveSettings(const InstancePaths& paths, const json::Value& settings, std::string& error) {
    if (!IsDir(paths.mod) && !CreateDirectories(paths.mod)) {
        error = "Could not create the stranded_together folder in the game folder.";
        return false;
    }
    return WriteAtomic(paths.settings, json::Dump(settings, true), true, error);
}

// ---- Collect logs -----------------------------------------------------------------------
bool CollectLogs(Env& env, const InstancePaths& paths, const std::string& summary, std::string& path_out, std::string& error) {
    path_out.clear();
    const std::wstring desktop = env.DesktopDir();
    if (desktop.empty() || !IsDir(desktop)) {
        error = "Your Desktop folder was not found.";
        return false;
    }
    ZipWriter zip;
    const Civil now = env.LocalTime(env.NowMs());
    std::uint16_t dos_time = 0, dos_date = 0;
    ToDosDateTime(now.year, now.month, now.day, now.hour, now.minute, now.second, dos_time, dos_date);
    zip.SetTimestamp(dos_time, dos_date);
    std::size_t added = 0;
    auto add_text = [&](const std::string& name, const std::string& text) {
        std::string e;
        if (zip.AddFile(name, text.data(), text.size(), e)) ++added;
    };
    struct Source {
        const std::wstring* path;
        const char* name;
        std::uint64_t max;
    };
    const Source sources[] = {
        {&paths.mod_log, "stranded_together.log", 8u << 20},
        {&paths.launcher_log, "launcher.log", 2u << 20},
        {&paths.launcher_old_log, "launcher.old.log", 2u << 20},
        {&paths.link_status, "link-status.json", 1u << 20},
        {&paths.record, "session-record.json", 1u << 20},
        {&paths.session_last_ini, "session.last.ini", 64u << 10},
        {&paths.package_json, "package.json", 64u << 10},
        {&paths.crash_report, "error.txt", 1u << 20},
    };
    // Only these files: never the side seats' pose cache (%LOCALAPPDATA%\StrandedTogether\mule_ride_lib.bin, game animation
    // data the mod learns at runtime), nor anything else of the game's.
    for (const Source& s : sources) {
        if (!IsFile(*s.path) || s.path->find(L"mule_ride_lib") != std::wstring::npos) continue;
        const std::string text = ReadTail(*s.path, s.max);
        add_text(s.name, Redact(text));
    }
    add_text("summary.txt", Redact(summary));
    std::vector<std::uint8_t> bytes;
    if (!zip.Finish(bytes, error)) return false;
    const std::wstring path = UniquePath(JoinPath2(desktop, L"stranded-together-logs-" + Wide(FormatStamp(now))), L".zip");
    if (!WriteAtomic(path, bytes.data(), bytes.size(), false, error)) return false;
    path_out = Utf8(path);
    (void)added;
    return true;
}

}  // namespace st::core
