// Stranded Together launcher core: folder layout and path rules (see core_paths.hpp).
#include "core_paths.hpp"

#include <algorithm>

namespace st::core {

InstancePaths GetInstancePaths(const std::wstring& game_dir) {
    InstancePaths p;
    p.game = FullPath(game_dir);
    p.exe = JoinPath2(p.game, kGameExe);
    p.steam_api = JoinPath2(p.game, L"steam_api64.dll");
    p.mod_dll = JoinPath2(p.game, kModDll);
    p.mod_dll_disabled = p.mod_dll + L".disabled";
    p.launcher_exe = JoinPath2(p.game, kLauncherExe);
    p.crash_report = JoinPath2(p.game, L"error.txt");
    p.mod = JoinPath2(p.game, kModFolder);
    auto m = [&](const wchar_t* name) { return JoinPath2(p.mod, name); };
    p.package_json = m(L"package.json");
    p.session_ini = m(L"session.ini");
    p.session_last_ini = m(L"session.last.ini");
    p.link_status = m(L"link-status.json");
    p.mod_log = m(L"stranded_together.log");
    p.record = m(L"session-record.json");
    p.settings = m(L"launcher-settings.json");
    p.launcher_log = m(L"launcher.log");
    p.launcher_old_log = m(L"launcher.old.log");
    return p;
}

std::string GamePathProblem(const std::wstring& game_root) {
    const std::wstring root = FullPath(game_root);
    if (IsDriveRoot(root))
        return "Stranded Together must be in the DEATH STRANDING game folder (the one with ds.exe), not at the top of a drive.";
    if (IsReparsePoint(root))
        return "This game folder is a link (junction or symbolic link). Put Stranded Together in the real game folder.";
    if (JoinPath2(root, L"stranded_together\\launcher-settings.json.00000000000000000000000000000000.tmp").size() > 259)
        return "The game folder path is too long for Windows (" + std::to_string(root.size()) +
               " characters). Move the game to a shorter folder path (Steam > Settings > Storage).";
    return {};
}

std::wstring PlayMutexName(const std::wstring& game_root) {
    std::wstring root = FullPath(game_root);
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/') && !IsDriveRoot(root)) root.pop_back();
    const std::string hash = Sha256Hex(Utf8(UpperInvariant(root)));
    return L"Local\\stranded-together-play-" + Wide(hash.substr(0, 16));
}

std::vector<std::wstring> SteamLibraryPathsFromVdf(const std::string& text) {
    std::vector<std::wstring> out;
    for (const std::string& raw : SplitLines(text)) {
        std::size_t p = 0;
        while (p < raw.size() && (raw[p] == ' ' || raw[p] == '\t')) ++p;
        if (p >= raw.size() || raw[p] != '"') continue;
        const std::size_t key_end = raw.find('"', p + 1);
        if (key_end == std::string::npos) continue;
        const std::string key = raw.substr(p + 1, key_end - p - 1);
        bool numeric = !key.empty();
        for (char c : key)
            if (c < '0' || c > '9') numeric = false;
        if (key != "path" && !numeric) continue;
        std::size_t q = key_end + 1;
        while (q < raw.size() && (raw[q] == ' ' || raw[q] == '\t')) ++q;
        if (q >= raw.size() || raw[q] != '"') continue;
        std::string value;
        bool closed = false;
        for (std::size_t i = q + 1; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                value.push_back(raw[i + 1]);
                ++i;
                continue;
            }
            if (raw[i] == '"') {
                closed = true;
                break;
            }
            value.push_back(raw[i]);
        }
        if (!closed) continue;
        const bool absolute = (value.size() >= 3 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= 'a' && value[0] <= 'z')) &&
                               value[1] == ':' && value[2] == '\\') ||
                              StartsWith(value, "\\\\");
        if (!absolute) continue;
        while (!value.empty() && (value.back() == '\\' || value.back() == '/')) value.pop_back();
        const std::wstring w = Wide(value);
        bool dup = false;
        for (const std::wstring& e : out)
            if (IEquals(e, w)) dup = true;
        if (!dup) out.push_back(w);
    }
    return out;
}

std::string InstallDirFromManifest(const std::string& manifest_text) {
    for (const std::string& raw : SplitLines(manifest_text)) {
        const std::string line = Trim(raw);
        if (!StartsWith(line, "\"installdir\"")) continue;
        const std::size_t q = line.find('"', 12);
        if (q == std::string::npos) continue;
        const std::size_t e = line.find('"', q + 1);
        if (e == std::string::npos || e == q + 1) continue;
        const std::string v = line.substr(q + 1, e - q - 1);
        if (v.find_first_of("\\/:*?<>|") != std::string::npos) continue;
        return v;
    }
    return kDefaultInstallDir;
}

std::vector<std::wstring> FindGameFolders(Env& env) {
    std::vector<std::wstring> roots, candidates, found;
    const wchar_t* steam_keys[][2] = {{L"0", L"Software\\Valve\\Steam"},
                                      {L"1", L"SOFTWARE\\WOW6432Node\\Valve\\Steam"},
                                      {L"1", L"SOFTWARE\\Valve\\Steam"}};
    for (const auto& k : steam_keys) {
        const bool hklm = k[0][0] == L'1';
        for (const wchar_t* name : {L"SteamPath", L"InstallPath"}) {
            std::wstring v;
            if (env.RegString(hklm, k[1], name, v) && !v.empty()) {
                std::replace(v.begin(), v.end(), L'/', L'\\');
                roots.push_back(v);
            }
        }
    }
    for (const wchar_t* key : {L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 1850570",
                               L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 1850570"}) {
        std::wstring v;
        if (env.RegString(true, key, L"InstallLocation", v) && !v.empty()) candidates.push_back(v);
    }
    auto add = [&](const std::wstring& folder) {
        if (folder.empty()) return;
        const std::wstring full = FullPath(folder);
        if (!IsFile(JoinPath2(full, kGameExe))) return;
        for (const std::wstring& e : found)
            if (IEquals(e, full)) return;
        found.push_back(full);
    };
    for (const std::wstring& c : candidates) add(c);
    for (const std::wstring& root : roots) {
        std::vector<std::wstring> libraries = {FullPath(root)};
        for (const wchar_t* vdf : {L"steamapps\\libraryfolders.vdf", L"config\\libraryfolders.vdf"}) {
            std::string text;
            if (ReadTextShared(JoinPath2(root, vdf), text, 4u << 20))
                for (const std::wstring& lib : SteamLibraryPathsFromVdf(text)) libraries.push_back(lib);
        }
        for (const std::wstring& lib : libraries) {
            std::string manifest, dir = kDefaultInstallDir;
            if (ReadTextShared(JoinPath2(lib, L"steamapps\\appmanifest_1850570.acf"), manifest, 1u << 20))
                dir = InstallDirFromManifest(manifest);
            add(JoinPath2(JoinPath2(lib, L"steamapps\\common"), Wide(dir)));
        }
    }
    return found;
}

}  // namespace st::core
