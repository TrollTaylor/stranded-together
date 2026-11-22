// Stranded Together launcher core: Fragile's textures (docs/textures.md). The Tools page's "Fragile's textures" card:
//  - where the folder is (<Documents>\Stranded Together\Textures, or the one picked with Change folder...),
//    remembered in launcher-settings.json "textures" {"dir": "" = the default, "previews": true};
//  - "Extract textures": the launcher starts ITS OWN exe as a child (--extract-textures, no window, stdout on a pipe),
//    so the game's oo2core never loads into the window's process and a crash there cannot take the window down;
//  - the status line and "Check my edits" (a scan of the folder on a worker thread; native/textures/extract_plan.hpp);
//  - session.ini textures_dir for the mod.
// The folder is only created by Extract / Open textures folder; nothing is written anywhere else.
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
// A folder the player may pick: absolute, not a drive root, not inside the game folder, not a Windows / Program
// Files folder. "" when fine, else a sentence.
std::string TexturesFolderProblem(const std::wstring& folder, const std::wstring& game_dir);

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
    std::string note;                // a "note ..." line (outdated seeds)
};
void ApplyExtractLine(const std::string& line, ExtractProgress& p);
std::string FormatMegabytes(std::uint64_t bytes);   // "330 MB"

// The command line of the child (the launcher's own exe).
std::wstring ExtractCommandLine(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews);

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

    bool StartExtract(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews, std::string& error);
    void Cancel();                    // TerminateProcess on our own child only
    bool extracting() const;
    ExtractProgress progress() const;
    // Finished since the last call (true once): the final progress and the child's exit code.
    bool TakeFinished(ExtractProgress& p, int& exit_code);

    // A scan of `dir` on a worker thread (full = Check my edits: every edited picture decoded).
    void StartScan(const std::wstring& dir, bool full);
    bool scanning() const;
    struct ScanResult {
        bool valid = false;           // a scan finished
        bool full = false;
        bool extracted = false, table_ok = false, outdated_game = false;
        bool extract_ok = true;       // the manifest's extractor output version is this launcher's (kExtractVersion)
        std::string extracted_at;
        int colour_folders = 0, boot_folders = 0, outdated_seeds = 0, problems = 0;
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
