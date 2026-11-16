// Stranded Together launcher core: the install gate, the Steam account, the
// launcher settings and Collect logs. Slimmed from Diamond Dogs Together's
// core_install: there is no setup step any more (no save backup, no runtime
// archive); the player extracts the release zip into the game folder and the
// launcher checks what it finds there.
//
// Gate:
//  - ds.exe next to the launcher (else the wrong-folder page);
//  - ds.exe / steam_api64.dll SHA-256 equal to the tested build: a mismatch
//    is a warning ("untested game version"), never a block;
//  - winhttp.dll (the mod) in the game folder and ours (CheckMod): its SHA-256
//    against stranded_together\package.json (a mismatch is a warning; without
//    package.json it is a development build); missing, switched off or another
//    program's winhttp.dll: co-op cannot start (a foreign one is never touched);
//  - Steam running with a logged-in user: Host and Steam joins need it.
#pragma once
#include <cstdint>
#include <string>

#include "core_env.hpp"
#include "core_json.hpp"
#include "core_paths.hpp"
#include "launcher_model.hpp"

namespace st::core {

// The DEATH STRANDING DIRECTOR'S CUT build the mod was made for (Steam build 13300582).
inline constexpr char kExeSha256[] = "9D5306D7EB2F5D3135C63ACCC3E74B8F9B029F9D76112A6AAE4A38FDBB4EDD2B";
inline constexpr char kSteamApiSha256[] = "F1EB582E607A1E43CDB1654BFB7CB29AD46F6728B3FB89A14F7727E0E8DAAB69";

struct GateResult {
    bool game_found = false;        // ds.exe exists
    bool exe_known = false;         // its SHA-256 is the tested one
    bool exe_hashed = false;        // the hash could be computed
    bool steam_api_known = false;
    bool mod_present = false;       // winhttp.dll exists (and is not a link)
    std::string path_problem;       // GamePathProblem, "" when fine
};
// Hashes ds.exe and steam_api64.dll; the result is cached in the settings by
// size and last-write time (ds.exe is 86 MB).
GateResult CheckGate(Env& env, const InstancePaths& paths, json::Value& settings);
// The sentence for a missing mod (how to install it).
std::string ModMissingText();
std::string UntestedVersionText(const GateResult& gate);   // "" when both hashes match

// ---- package.json and winhttp.dll ----------------------------------------------------
// stranded_together\package.json, written by tools/package.ps1 into the release zip:
//   {"product":"stranded-together-package","v":1,"version":"0.1.0","build_id":"0.1.0+abc1234",
//    "built_utc":"...","source_commit":"<40 hex>"|null,"source_dirty":false,
//    "files":[{"path":"winhttp.dll","sha256":"<64 hex>","size":n},
//             {"path":"StrandedTogether.exe","sha256":"<64 hex>","size":n}, ...]}
inline constexpr char kPackageProduct[] = "stranded-together-package";
struct PackageInfo {
    bool present = false;        // the file exists
    bool ok = false;             // it parses, is ours and names exactly one winhttp.dll hash
    std::string problem;         // present && !ok: why
    std::string version, build_id, source_commit;
    std::string dll_sha256;      // upper case
    std::string exe_sha256;      // upper case, "" when not listed
};
PackageInfo ReadPackage(const InstancePaths& paths);
PackageInfo ParsePackageText(const std::string& text);
// The file is a PE image that forwards winhttp's exports to System32 through
// \.\GLOBALROOT\SystemRoot\System32\winhttp.dll (the Stranded Together proxy)
// and names its stranded_together folder. Another mod's or loader's winhttp.dll does not.
bool LooksLikeOurDll(const std::wstring& path);
bool LooksLikeOurDllBytes(const std::string& bytes);
struct ModCheck {
    ModState state = ModState::missing;
    std::string note;            // the player's sentence for mismatch / foreign / disabled / dev_build
    std::string dll_sha256;
};
// `launcher_version` is this program's build id: a package of another build is a mismatch.
ModCheck CheckMod(const InstancePaths& paths, const PackageInfo& package, const std::string& launcher_version);
// The sentences for the Play page's not-ready card.
std::string ModForeignText();
std::string ModDisabledText();
// Debug > Disable / Enable mod: renames winhttp.dll <-> winhttp.dll.disabled. Only
// our own DLL is ever moved, never while ds.exe runs from the folder, and never
// over an existing file.
bool SetModEnabled(Env& env, const InstancePaths& paths, const PackageInfo& package, bool enable, std::string& error);

// ---- Steam account ----------------------------------------------------------------
std::uint32_t SteamActiveUser(Env& env);   // HKCU\Software\Valve\Steam\ActiveProcess\ActiveUser, 0 = none
std::uint64_t OwnSteamId(Env& env);        // 0 when nobody is logged in
// Steam's client process runs (ActiveProcess\pid alive) and a user is logged in.
bool SteamClientRunning(Env& env);
// Steam's own "app running" flag for DEATH STRANDING (HKCU\...\Apps\1850570\Running).
bool SteamAppRunningFlag(Env& env);

// ---- launcher-settings.json -----------------------------------------------------------
bool ReadSettings(const InstancePaths& paths, json::Value& out);    // an object, empty when absent or invalid
bool SaveSettings(const InstancePaths& paths, const json::Value& settings, std::string& error);

// ---- Collect logs -----------------------------------------------------------------------
// A zip on the Desktop (stranded-together-logs-<stamp>.zip): the mod's log, the
// launcher logs, link-status.json and the session record, every text redacted
// (no join codes, secrets, SteamIDs, user names), plus summary.txt.
bool CollectLogs(Env& env, const InstancePaths& paths, const std::string& summary, std::string& path_out, std::string& error);

}  // namespace st::core
