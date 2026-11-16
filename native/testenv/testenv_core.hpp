// Shared pieces of the hidden test environment (docs/hidden-testing.md):
// files and hashes, the scenario language, the session.ini of a hidden run,
// backup/restore of the game folder, process lookup. No windows, no desktops.
#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace st::testenv {

// ------------------------------------------------------------------ text / files
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& w);
std::string JsonEscape(const std::string& s);
bool ReadFileBytes(const std::wstring& path, std::string* out, std::uint64_t max_bytes = 64ull << 20);
bool WriteFileAtomic(const std::wstring& path, const std::string& bytes);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);  // creates parents
bool RemoveTree(const std::wstring& path);  // files and directories below, then the directory
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::wstring FullPath(const std::wstring& p);
// Lower-case hex SHA-256 of a file, "" when unreadable.
std::string Sha256File(const std::wstring& path);
// Files below root (relative paths, '\\'-separated), sorted. Skips reparse points.
std::vector<std::wstring> ListFiles(const std::wstring& root, bool recursive);
bool CopyTree(const std::wstring& from, const std::wstring& to);
std::string NowStamp();  // yyyyMMdd-HHmmss local time
std::string IsoNow();    // 2026-11-16T12:00:00.123

// ------------------------------------------------------------------ session.ini
// role solo|echo; loopback UDP only (nothing reaches Steam networking), one-shot.
// isolate=false writes automation_isolate=0 (channel on, overlay automation hooks off).
std::string SessionIniText(const std::string& role, bool mute, bool isolate = true);

// ------------------------------------------------------------------ scenarios
// One line each:
//   # comment / blank
//   role solo|echo               session role (default solo)
//   timeout <s>                  whole run limit, 30..360 (default 360)
//   waitready <s>                until the mod's automation channel is up
//   waitlog <s> <regex>          until a stranded_together.log line matches (ECMAScript)
//   until <s> <every_ms> <regex> => <mod command>
//                                send the command every N ms until the log matches
//   sleep <ms>                   supervisor-side pause
//   note <text>                  a line in supervisor.log
//   <anything else>              one mod command (native/mod/automation.hpp), checked here
// A leading '?' makes a step optional: its failure is recorded, the run goes on.
// Any other failed step ends the scenario (the supervisor still screenshots and quits).
enum class StepKind { mod, waitready, waitlog, until, sleep, note };
struct Step {
    StepKind kind = StepKind::mod;
    int line = 0;
    bool optional = false;
    std::string text;           // mod command / regex / note
    std::string command;        // until: the mod command
    std::uint32_t seconds = 0;  // waitready / waitlog / until
    std::uint32_t ms = 0;       // sleep / until interval
};
struct Scenario {
    std::string role = "solo";
    unsigned timeout_s = 360;
    std::vector<Step> steps;
};
bool ParseScenario(const std::string& text, Scenario* out, std::string* error);
// Seconds the supervisor waits for the ack of one mod command.
unsigned AckTimeoutSeconds(const std::string& mod_command);

// ------------------------------------------------------------------ game folder guard
// Everything a hidden run may change in the game folder is recorded in
// <run>\restore.txt BEFORE the first change, with backups in <run>\backup\:
//   <game>\winhttp.dll, <game>\error.txt, <game>\steam_appid.txt (never written:
//   checked), every top-level file of <game>\stranded_together, and the
//   automation folder (removed afterwards). Restore puts each recorded file back
//   (or deletes it when it did not exist) and moves files that appeared in
//   stranded_together to <run>\game-new\. Restore works from the manifest alone
//   (supervisor --restore <run> after a crash).
struct GuardResult {
    bool ok = false;
    std::vector<std::string> notes;  // what was restored / removed / kept
};
bool BackupGameFolder(const std::wstring& game_dir, const std::wstring& run_dir, std::string* error);
GuardResult RestoreGameFolder(const std::wstring& run_dir);

// ------------------------------------------------------------------ saves
// path \t size \t sha256 lines for every file below dir.
std::string HashManifest(const std::wstring& dir);
// Lines "changed|added|removed <path>" between two manifests.
std::vector<std::string> DiffManifests(const std::string& before, const std::string& after);

// ------------------------------------------------------------------ processes
std::vector<DWORD> FindProcesses(const wchar_t* exe_name);  // case-insensitive image name

}  // namespace st::testenv
