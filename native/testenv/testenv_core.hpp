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
// save_root (tests): an extra save root the mod redirects (automation_save_root=).
// foreign_saves (only with --seed-saves): automation_foreign_saves=1, the mod skips ds.exe's
// save-owner check so another account's saves load (automation.hpp "foreign saves").
std::string SessionIniText(const std::string& role, bool mute, bool isolate = true, const std::wstring& save_root = {},
                           bool foreign_saves = false);
// Every field: the two-game runs (role host: the first game role=host on 27555->27556, the
// second role=guest on 27556->27555 with second_instance=1) and appdir_settings (graphics low).
struct SessionOptions {
    std::string role = "solo";
    bool mute = true, isolate = true;
    std::wstring save_root;
    bool foreign_saves = false;
    bool appdir_settings = false;   // appdir_settings=1: <game>\settings.cfg -> automation\settings.cfg
    bool second_instance = false;   // second_instance=1: the second ds.exe's single-instance mutex is renamed
    int port_local = 27555, port_peer = 27556;
    unsigned frame_cap = 0;         // frame_cap=<fps> (graphics low: 30; the mod waits once per frame)
    std::string local_window;       // local_window=left|right (two-game runs: the windows side by side)
    // scenario "start": automation_quickstart=<spec> (quickstart.hpp: "continue", "load <type> <index>", "story <n>"). Written by
    // the supervisor only (never an allow-listed "ini" key), into the launcher's file too (launcherini).
    std::string quickstart;
    std::vector<std::string> extra;  // scenario "ini key=value" lines (allow-listed keys only)
};
std::string SessionIniText(const SessionOptions& options);
// The supervisor's rule: automation_foreign_saves=1 only when --seed-saves names a seed and
// the full mod runs (--no-mod installs no game patches).
inline bool WantForeignSaves(const std::wstring& seed, bool no_mod) { return !seed.empty() && !no_mod; }
// --sandbox-session runs: the session.ini the launcher writes for "Play with test save"
// (no automation=1: sandbox_saves=<name>, sandbox_foreign_saves with a seed, no debug menu), so
// the mod's visible-play sandbox path runs in a hidden game. No automation channel.
std::string SandboxSessionIniText(const std::string& name, bool foreign_saves);
// --no-mod runs: the mod only redirects the saves (no role, no overlay, no game hooks).
std::string RedirectOnlyIniText(const std::wstring& save_root = {});
// --launcher-ini runs (scenario "launcherini local-test"; Start-TestRun.ps1 writes the files with
// StrandedTogether.exe --write-session local-test): the launcher's own session.ini for one game, kept line for line,
// except where the saves are: sandbox_saves (a folder in the game folder's stranded_together\sandbox) and
// sandbox_foreign_saves become the run's automation redirect (automation=1, automation_foreign_saves=1 with a seed,
// automation_save_root). The launcher's header comment is replaced (a launcher window would take a file with it for its
// own leftover). Then the automation channel's keys (automation_mute, automation_isolate). Every other key
// (host_save, partner_body, my_character, local_input, frame_cap, appdir_settings, second_instance, ...) is the
// launcher's. False (with *error) for a file that is not the launcher's, has no role, or already carries automation keys.
// *replaced: the launcher lines that were replaced (for supervisor.log).
bool HiddenIniFromLauncher(const std::string& launcher_text, const SessionOptions& options, std::string* out,
                           std::vector<std::string>* replaced, std::string* error);
// v0.23: the game frame rate the launcher's Local test writes into each sandbox's settings.cfg, from its host.ini's
// frame_cap (the same choice: 30 / 60 / 120; 0 or none = its "Uncapped", the game's maximum 240). launcherini runs give
// it to "graphics low" unless the scenario names a rate ("graphics low <fps>").
unsigned LauncherIniGameFps(const std::string& launcher_text);

// ------------------------------------------------------------------ named places (tools/testenv/places.txt)
// One place a line: "<name> <area> <x> <y> <z> [# note]" (name [a-z0-9-], 1..32 chars; z 0 = the ground under x, y). The
// scenario's "goto <name>" reads it (supervisor --places <file>; Start-TestRun.ps1 passes tools\testenv\places.txt).
struct Place {
    std::string name;
    int area = 0;
    double x = 0, y = 0, z = 0;
    std::string note;
};
bool ParsePlaces(const std::string& text, std::vector<Place>* out, std::string* error);
const Place* FindPlace(const std::vector<Place>& places, const std::string& name);

// ------------------------------------------------------------------ scenarios
// One line each:
//   # comment / blank
//   role solo|echo|host          session role (default solo); host: a second game (guest) is started
//                                too, on the same private desktop (two-game run)
//   graphics low                 both games read sandbox.hpp's low-quality windowed settings.cfg
//   @guest <step>                the step goes to the second game (role host); @both <mod command> to both
//   launcherini local-test       role host only: both games run the session.ini the launcher's Local test writes
//                                (supervisor --launcher-ini <dir>, HiddenIniFromLauncher); no "ini" lines then
//   timeout <s>                  whole run limit, 30..360 (default 360)
//   waitready <s>                until the mod's automation channel is up
//   waitlog <s> <regex>          until a stranded_together.log line matches (ECMAScript)
//   until <s> <every_ms> <regex> => <mod command>
//                                send the command every N ms until the log matches
//   sleep <ms>                   supervisor-side pause
//   start [continue | load <type> <index> | story <n>] [control <s>]
//                                quickstart (docs/hidden-testing.md "start"): no menu steps. The supervisor writes
//                                automation_quickstart=<spec> (default continue) into every game's session.ini; the mod calls the
//                                title's own Continue / Load once its title page has been up 3 s (a host-held save guest waits for
//                                the host's save). Expands, where the line stands, to "waitready 90" and "waitworld <s>" (default
//                                120) for every game (@both in a two-game run). Once per scenario.
//   goto <place> | goto <x> <y> [<z>]
//                                Sam to a named place (places.txt) or coordinates (z 0 / left out: the ground under x, y),
//                                then the ground streams in: "action dev_goto x y z" + "waitworld 45 goto". Sandboxed saves only
//                                (every hidden run). "@guest goto" moves the other game's Sam, "@both goto" both.
//   waitnew <s> <regex>          as waitlog, but only log lines written since the PREVIOUS step started (an action's own
//                                lines count; older matches never do: no counted regexes needed)
//   note <text>                  a line in supervisor.log
//   <anything else>              one mod command (native/mod/automation.hpp), checked here
// A leading '?' makes a step optional: its failure is recorded, the run goes on.
// Any other failed step ends the scenario (the supervisor still screenshots and quits).
// nolog: fails when the log already has a matching line (a negative check, no waiting).
enum class StepKind { mod, waitready, waitlog, nolog, until, sleep, note, waitnew };
struct Step {
    StepKind kind = StepKind::mod;
    int line = 0;
    bool optional = false;
    std::string text;           // mod command / regex / note
    std::string command;        // until: the mod command
    std::uint32_t seconds = 0;  // waitready / waitlog / until
    std::uint32_t ms = 0;       // sleep / until interval
    int target = 0;             // 0 the first game, 1 the second (guest), 2 both (mod commands, waitready)
};
struct Scenario {
    std::string role = "solo";
    bool low_graphics = false;  // graphics low
    unsigned low_graphics_fps = 30;  // graphics low <fps>: the game's own "fps" / "limit_fps" (30..240)
    bool low_graphics_fps_set = false;  // ... named in the scenario (else launcherini runs take the launcher's rate)
    unsigned timeout_s = 360;
    std::vector<std::string> ini;        // "ini key=value": extra session.ini lines (partner/pose keys only), every game
    std::vector<std::string> host_ini;   // "@host ini key=value": the first game only, written after `ini`
    std::vector<std::string> guest_ini;  // "@guest ini key=value": the guest game only (role host), after `ini`
    int guest_ini_line = 0;              // for the role-host check's error message
    // "guest unredirected" (role host): the guest's session.ini is the host-held save session's guest WITHOUT any save
    // redirect (no automation=1, no sandbox_saves; role=guest host_save=1). The run expects the guest's mod to end
    // ds.exe in DllMain (exit 0x53540006, sandbox-refused.txt) before ds.exe's own code runs; no @guest step may follow.
    bool guest_unredirected = false;
    int guest_unredirected_line = 0;
    // "launcherini local-test": the two games' session.ini are the launcher's own (the supervisor refuses the run without
    // --launcher-ini, and --launcher-ini without this line).
    std::string launcher_ini;
    int launcher_ini_line = 0;
    // "start": the quickstart spec ("" = no start line) and how long the world may take
    std::string quickstart;
    int quickstart_line = 0;
    std::uint32_t quickstart_control_s = 120;
    std::vector<Step> steps;
};
// Keys a scenario may add to session.ini with "ini key=value" (nothing that touches saves,
// automation, isolation or instances).
bool ScenarioIniKeyAllowed(const std::string& key);
// The guest session.ini of "guest unredirected": role=guest, host_save=1, loopback ports, second_instance=1, and no
// automation / sandbox key at all (the mod must refuse it in DllMain).
std::string UnredirectedGuestIniText(unsigned port_local, unsigned port_peer);
// places: the named places "goto <name>" may use (nullptr: none, only coordinates).
bool ParseScenario(const std::string& text, Scenario* out, std::string* error, const std::vector<Place>* places = nullptr);
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
// path \t size \t sha256 lines for every file below dir, and "path\ \t dir \t -" for
// every directory (an empty folder the game creates is a change too).
std::string HashManifest(const std::wstring& dir);
// Lines "changed|added|removed <path>" between two manifests.
std::vector<std::string> DiffManifests(const std::string& before, const std::string& after);

// The user's save tree (default %LOCALAPPDATA%\KojimaProductions; docs/hidden-testing.md
// "Saves"). Before a run BackupSaves hashes it into <run>\saves-before.txt, copies it to
// <run>\saves-backup\ (verified against the manifest) and writes <run>\saves-restore.txt
// (root, backup) BEFORE the game starts. SeedScratch copies the backup into the scratch
// folder the mod redirects to (<game>\stranded_together\automation\saves), verified, plus
// the mod's self-test probe file. After the run CheckAndRestoreSaves compares the real
// tree with saves-before.txt; anything changed or removed is copied back from the backup,
// files that appeared are moved to <run>\saves-added\ (never deleted), folders that
// appeared are removed when empty; then it verifies again and writes saves-restored.txt.
bool BackupSaves(const std::wstring& root, const std::wstring& run_dir, std::string* error);
bool SeedScratch(const std::wstring& run_dir, const std::wstring& scratch, std::string* error);
// --seed-saves: replace the save slots of every Steam account folder in the seeded scratch
// copy (scratch\DeathStrandingDC\<digits>\) with <seed>\DeathStrandingDC\* slot folders
// (autosave*/manualsave*/quicksave*). Everything else in the account folder (profile\,
// game_settings.cfg, steam_autocloud.vdf) stays the user's. Only ever touches the scratch
// copy. *accounts = how many account folders were seeded.
bool ApplySeedSaves(const std::wstring& scratch, const std::wstring& seed, int* accounts, std::string* error);
// --guest-seed profile-only (the host-held save session, docs/coop-session.md): the guest game's scratch copy keeps
// only what is not a save slot (profile\, the settings files) in every Steam account folder: every
// autosave*/manualsave*/quicksave* folder is deleted, as the launcher's coop-guest folder has none at a session's
// start. Only ever touches the scratch copy (refused unless `scratch` has the redirect probe). *slots = folders removed.
bool StripScratchSlots(const std::wstring& scratch, int* slots, std::string* error);
// HashManifest(dir) without the redirect probe line (the probe is deleted before the after-hash).
std::string ManifestWithoutProbe(const std::wstring& dir);
struct SavesCheck {
    bool ran = false;                  // saves-restore.txt was there
    bool unchanged = false;            // the real tree matched the manifest before any restore
    bool ok = false;                   // matches now (unchanged, or restored and verified)
    std::vector<std::string> changes;  // diff found before restoring
    std::vector<std::string> notes;    // what was restored / moved
};
SavesCheck CheckAndRestoreSaves(const std::wstring& run_dir);

// Lines of a Steam cloud_log.txt excerpt that concern `app_id` ("[AppID 1850570]").
std::vector<std::string> CloudLogLines(const std::string& log_text, const std::string& app_id);
// Of those, the ones that upload or would upload a file ("Need to upload file", "Upload
// OK for file", "HTTP upload for file"): with the redirect there must be none.
std::vector<std::string> CloudUploads(const std::vector<std::string>& app_lines);
// The mod's online gate (docs/world-sync.md): every "[gate] OTGC WRITE request: <name>" line of a game's log (a request that
// writes to the game server: a hidden run must have none), and the largest "(total N)" of its "[gate] ... dropped ..." count
// lines (the online calls the gate dropped).
std::vector<std::string> GameServerWrites(const std::string& log_text);
unsigned OnlineBlocked(const std::string& log_text);
// Round 5: whether the gate went in: "on" for "[gate] online gate ON", "off: <reason>" for "[gate] OFF: <reason>",
// "inactive: ..." when the mod stopped at start-up (an unsupported ds.exe: no tick, nothing can run), "" when the log has
// none of these. Without "on" (or "inactive"), GameServerWrites cannot be trusted (no OTGC logger).
std::string GateState(const std::string& log_text);

// ------------------------------------------------------------------ processes
std::vector<DWORD> FindProcesses(const wchar_t* exe_name);  // case-insensitive image name

}  // namespace st::testenv
