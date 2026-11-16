// Stranded Together launcher core: the folder layout of one DEATH STRANDING
// DIRECTOR'S CUT game folder, the path rules, the per-folder mutex name and
// Steam's game-folder discovery. Adapted from Diamond Dogs Together's
// core_paths (same algorithm, Death Stranding constants).
#pragma once
#include <string>
#include <vector>

#include "core_env.hpp"

namespace st::core {

inline constexpr wchar_t kGameExe[] = L"ds.exe";
inline constexpr wchar_t kModDll[] = L"winhttp.dll";          // the mod: a winhttp proxy ds.exe loads from its folder
inline constexpr wchar_t kModFolder[] = L"stranded_together";
inline constexpr wchar_t kLauncherExe[] = L"StrandedTogether.exe";
inline constexpr char kDefaultInstallDir[] = "DEATH STRANDING DIRECTORS CUT";

// G = the game folder, M = G\stranded_together.
struct InstancePaths {
    std::wstring game, exe, steam_api, mod_dll;
    std::wstring mod_dll_disabled;   // G\winhttp.dll.disabled (Debug > Disable mod)
    std::wstring launcher_exe;       // G\StrandedTogether.exe
    std::wstring crash_report;       // G\error.txt            (the game's own crash reporter)
    std::wstring mod;            // M
    std::wstring package_json;   // M\package.json           (the release zip: version and file hashes)
    std::wstring session_ini;    // M\session.ini            (launcher -> mod)
    std::wstring session_last_ini;   // M\session.last.ini   (the mod renames a one-shot session.ini to this)
    std::wstring link_status;    // M\link-status.json       (mod -> launcher)
    std::wstring mod_log;        // M\stranded_together.log  (the mod's own log)
    std::wstring record;         // M\session-record.json    (the launcher's session journal)
    std::wstring settings;       // M\launcher-settings.json
    std::wstring launcher_log, launcher_old_log;
};
InstancePaths GetInstancePaths(const std::wstring& game_dir);

// "" when the game folder can hold the co-op files, otherwise a sentence
// (a drive root, a link).
std::string GamePathProblem(const std::wstring& game_root);

// Local\stranded-together-play-<first 16 hex of SHA-256(upper-case game root)>.
std::wstring PlayMutexName(const std::wstring& game_root);

// Steam's DEATH STRANDING folders (read-only): registry Steam roots, the
// uninstall key's InstallLocation, libraryfolders.vdf libraries and
// appmanifest_1850570.acf installdir; folders that contain ds.exe, first
// found first.
std::vector<std::wstring> SteamLibraryPathsFromVdf(const std::string& text);
std::string InstallDirFromManifest(const std::string& manifest_text);   // default kDefaultInstallDir
std::vector<std::wstring> FindGameFolders(Env& env);

}  // namespace st::core
