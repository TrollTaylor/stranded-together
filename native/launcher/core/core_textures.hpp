// Stranded Together launcher core: Fragile's textures (docs/textures.md). The Tools page's "Fragile's textures" card:
//  - where the folder is (<Documents>\Stranded Together\Textures, or the one picked with Change folder...),
//    remembered in launcher-settings.json "textures" {"dir": "" = the default, "previews": true};
//  - "Extract textures": the launcher starts ITS OWN exe as a child (--extract-textures, no window, stdout on a pipe),
//    so the game's oo2core never loads into the window's process and a crash there cannot take the window down;
//  - the status line and "Check my edits" (a scan of the folder on a worker thread; native/textures/extract_plan.hpp);
//  - session.ini textures_dir for the mod.
//  - the automatic extraction (round 9): the same child, started by the real window when the folder was never
//    extracted or by an older version (DecideAutoExtract);
// The folder is only created by an extraction (by hand or automatic) / Open textures folder; nothing is written anywhere else.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core_json.hpp"

namespace st::core {

// This program's own exe (the --extract-textures child is started from it).
std::wstring SelfExePath();

// <Documents>\Stranded Together\Textures (SHGetKnownFolderPath(FOLDERID_Documents)) when Documents is a local folder that
// OneDrive does not sync; else <LocalAppData>\Stranded Together\Textures (the extracted game pictures and the edits are
// never uploaded, and a network Documents cannot stop a session). "" when Windows names neither.
std::wstring DefaultTexturesDir();
// True when `folder` is inside OneDrive (FOLDERID_SkyDrive, the OneDrive / OneDriveConsumer / OneDriveCommercial roots).
bool InOneDrive(const std::wstring& folder);
// The folder the settings name ("textures"."dir"), or the default.
std::wstring TexturesDirFromSettings(const json::Value& settings);
// session.ini textures_dir: the folder of the settings when the mod can use it (TexturesFolderProblem, at most 1024 bytes),
// else "" (with `why`): a folder that cannot be used never stops a session from starting, it only means no textures.
std::string TexturesDirForSession(const json::Value& settings, const std::wstring& game_dir, std::string* why = nullptr);
// A line for the card about the folder ("" = nothing to say): synced by OneDrive (a picked folder), the default moved out
// of a synced / network Documents, or why the mod cannot use it.
std::string TexturesFolderNote(const std::wstring& folder, bool custom, const std::wstring& game_dir);
bool TexturePreviewsFromSettings(const json::Value& settings);   // "textures"."previews" (default on)
// Her boots (session.ini boots_look): "textures"."boots" = "own" (default: her own boots, painted with the boot pictures)
// or "prop" (Sam's boots on her feet). Anything else reads as "own".
std::string BootsLookFromSettings(const json::Value& settings);
// Fragile in the private room (session.ini room_outfit, docs/real-partner-body.md section 34): "textures"."room" = "black"
// (default: her black suit there) or "suit" (her porter suit there too). Anything else reads as "black".
std::string RoomOutfitFromSettings(const json::Value& settings);
// A folder the player may pick: absolute, not a drive root, not inside the game folder, not a Windows / Program
// Files folder. "" when fine, else a sentence.
std::string TexturesFolderProblem(const std::wstring& folder, const std::wstring& game_dir);
// Ready-made colours (round 8, docs/textures.md "Ready-made colours"): "textures"."defaults" (default on). Off writes
// session.ini textures_defaults=0 (a folder the player did not paint is then her own suit).
bool ReadyMadeFromSettings(const json::Value& settings);

// ---- Automatic extraction (round 9, docs/textures.md "Get the pictures") ----------------------------------------
// The real window's launcher fills the textures folder by itself, in the background, with the same child process as
// Extract textures, when the folder was never extracted or was extracted by an older Stranded Together (the manifest's
// extract version or slot table). It never waits on play: Host / Join / Local test start whenever the player likes;
// a game that starts meanwhile shows Fragile's own suit until the manifest is written, then the mod's watch of the
// folder picks the ready-made colours up. "textures"."auto_extract" (default on) turns it off.
bool AutoExtractFromSettings(const json::Value& settings);

// What the folder's .manifest.txt says (a small file, read on the UI thread).
struct ManifestFacts {
    bool present = false;       // the file exists
    bool readable = false;      // and parses
    int extract = 0;            // its extract version (extract_plan.hpp kExtractVersion)
    bool table_ok = false;      // its slot table is this build's (tex_slots.hpp kTableVersion)
    bool open_failed = false;   // it exists but could not be opened right now (another program holds it): look again later
};
ManifestFacts ReadManifestFacts(const std::wstring& dir);

// The folder this launcher last saw extracted ("textures"."extracted", lower case): a folder that loses its index after
// that (deleted to free space, its drive unplugged or given to another device) is never filled again by itself.
bool WasExtractedHere(const json::Value& settings, const std::wstring& dir);
// true when the settings changed (then saved by the caller).
bool RememberExtractedHere(json::Value& settings, const std::wstring& dir);
// Free bytes on the drive of `dir` (its nearest existing parent); UINT64_MAX when unknown.
std::uint64_t FreeBytesFor(const std::wstring& dir);
// An automatic run needs this much free (the extraction's own 400 MB floor plus 1 GB left for Windows and the game); a
// drive with less waits for the player (Extract textures keeps the 400 MB floor).
inline constexpr std::uint64_t kAutoExtractMinFreeBytes = (400ull + 1024ull) << 20;

// Automatic runs that failed, per target (persisted as "textures"."auto": {"key", "failures", "last_s", "error"}, so a
// launcher restarted at once does not try again at once). The key names what was being made: this build's version and
// extract version, the game's ds.exe stamp and the folder; a new key (a Stranded Together or game update, another folder)
// starts from zero. Extract textures (by hand) and a finished automatic run clear it.
struct AutoExtractRecord {
    std::string key;
    int failures = 0;
    std::int64_t last_s = 0;   // when the last one failed (Unix seconds)
    std::string error;         // its sentence
};
std::string AutoExtractKey(const std::wstring& dir, const std::string& game_stamp);
AutoExtractRecord AutoExtractRecordFromSettings(const json::Value& settings);
void StoreAutoExtractRecord(json::Value& settings, const AutoExtractRecord& r);   // an empty key removes it

// After the 1st failure the next automatic try waits 10 minutes, after the 2nd 1 hour; after the 3rd there is none for
// that key (the card says so once, quietly: no dialog). -1 = no more automatic tries.
inline constexpr int kAutoExtractMaxFailures = 3;
std::int64_t AutoExtractRetryAfterS(int failures);

struct AutoExtractFacts {
    bool enabled = true;          // "textures"."auto_extract"
    bool folder_usable = false;   // a folder is named and TexturesFolderProblem has nothing against it
    bool onedrive = false;        // inside OneDrive: never filled automatically (about 330 MB would be uploaded)
    bool extracting = false;      // an extraction runs (by hand or automatic)
    bool game_running = false;    // a game of this folder, Local test, or a session being started or played
    ManifestFacts manifest;
    bool cancelled = false;       // the player cancelled an automatic run since this launcher started
    int failures = 0;             // automatic runs that failed for this key (AutoExtractRecord)
    std::int64_t now_s = 0, last_failure_s = 0;
    bool was_extracted = false;   // WasExtractedHere: the folder held an extraction before (its loss is the player's call)
    bool low_disk = false;        // less than kAutoExtractMinFreeBytes free (looked at only when it would start)
};
enum class AutoExtract {
    not_needed,    // extracted by this version (or a newer one)
    off,           // the setting is off
    no_folder,     // no folder, or one the mod cannot use
    onedrive,      // a folder OneDrive syncs
    busy,          // an extraction runs
    cancelled,     // cancelled by the player: not again until the launcher starts again
    gave_up,       // kAutoExtractMaxFailures failures for this key
    retry_later,   // a failure: the next try at retry_at_s
    wait_game,     // a game runs: after it closes (an automatic run never starts under a running game)
    start,         // start it now
    look_again,    // the index exists but could not be opened right now: looked at again shortly (never a start on it)
    unreadable,    // the index does not read (damaged, or written by a newer Stranded Together): the player's call, as
                   // an extraction then has no record of which pictures are unpainted
    gone,          // the folder held an extraction and lost it (deleted, a missing drive): the player's call
    low_disk,      // not enough free space for an automatic run
};
struct AutoExtractDecision {
    AutoExtract what = AutoExtract::not_needed;
    std::string reason;            // why it is needed (launcher.log): "never extracted", "extracted by an older Stranded Together (extract 2, now 3)"
    std::string user_reason;       // the same in the card's words: "not extracted yet", "Stranded Together was updated"
    std::int64_t retry_at_s = 0;   // retry_later
};
// A start, or one that comes by itself later (busy, waiting for the game or for a retry): the card may say "by itself".
bool AutoExtractWillRun(AutoExtract what);
AutoExtractDecision DecideAutoExtract(const AutoExtractFacts& f);

// Automation (--allow-out-under): "" when `out` may be written by a test, else why not. Refuses the user's Documents,
// Desktop, Pictures and OneDrive wherever they are, and anything in the user profile outside the temp folder.
std::string AutomationOutProblem(const std::wstring& out);

// One line of the child's output (--extract-textures): progress / skip / kept / note / set / done / error.
struct ExtractProgress {
    int done = 0, total = 0;
    std::string current;             // what it works on
    int sets = 0, skipped = 0, kept = 0, written = 0;
    std::uint64_t bytes = 0, ms = 0;
    bool finished = false;           // a done line came
    std::string error;               // an "error <sentence>" line
    std::string note;                // the "note ..." lines, joined (outdated seeds, ready-made colours not made, left behind)
    bool defaults_not_made = false;  // a "note ready-made colours not made: ..." line came
    int incomplete = 0;              // "incomplete <n> <extract>": unpainted pictures left for the next extraction
    // "defaults <written> <replaced> <yours> <ms>": the ready-made colours' pictures created / refreshed, painted folders kept
    bool defaults_seen = false;
    int defaults_written = 0, defaults_replaced = 0, folders_yours = 0;
    std::uint64_t defaults_ms = 0;
};
void ApplyExtractLine(const std::string& line, ExtractProgress& p);
std::string FormatMegabytes(std::uint64_t bytes);   // "330 MB"

// The command line of the child (the launcher's own exe). allow_under: automation only (--allow-out-under; tests).
std::wstring ExtractCommandLine(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews,
                                const std::wstring& allow_under = {});

// --extract-textures in the child (and for tests / the hidden check): validates --out, runs the extraction and prints
// the lines through `print`. allow_under: automation only, the out folder must be inside it and never inside the
// user's Documents; without it the out folder must be the one the launcher's settings name.
struct ExtractRequest {
    std::wstring game_dir;
    std::wstring out_dir;
    std::wstring allow_under;
    bool previews = true;
    int cancel_after_sets = -1;
    bool check_only = false;         // --check: the Check my edits report instead of an extraction
};
int RunExtractTextures(const ExtractRequest& r, const std::function<void(const std::string&)>& print);

// The card's background work: the child process and the folder scan. Owned by the real model; every member is
// called from the UI thread.
class TextureTasks {
public:
    TextureTasks();
    ~TextureTasks();
    TextureTasks(const TextureTasks&) = delete;
    TextureTasks& operator=(const TextureTasks&) = delete;

    bool StartExtract(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews, std::string& error,
                      const std::wstring& allow_under = {});
    void Cancel();                    // TerminateProcess on our own child only
    bool extracting() const;
    ExtractProgress progress() const;
    // Finished since the last call (true once): the final progress and the child's exit code.
    bool TakeFinished(ExtractProgress& p, int& exit_code);

    // A scan of `dir` on a worker thread (full = Check my edits: every edited picture decoded).
    // ready_made: READY-MADE COLOURS (the Check my edits report's wording).
    void StartScan(const std::wstring& dir, bool full, bool ready_made = true);
    bool scanning() const;
    struct ScanResult {
        bool valid = false;           // a scan finished
        bool full = false;
        bool extracted = false, table_ok = false, outdated_game = false;
        bool extract_ok = true;       // the manifest's extractor output version is this launcher's (kExtractVersion) or newer
        std::string extracted_at;
        int colour_folders = 0, boot_folders = 0, outdated_seeds = 0, problems = 0;
        int ready_made = 0, stale_ready_made = 0, painted_colours = 0;   // colour folders 01..16: nothing painted / older / painted
        std::string edited;           // EditSummary
        std::string report;           // CheckReport
        std::string game;             // the manifest's game stamp
    };
    ScanResult scan() const;

private:
    void ReaderLoop(void* pipe);
    mutable std::mutex mu_;
    void* process_ = nullptr;
    std::thread reader_, scanner_;
    std::atomic<bool> extracting_{false}, finished_{false}, scanning_{false};
    ExtractProgress progress_;
    int exit_code_ = 0;
    ScanResult scan_;
};

}  // namespace st::core
