// Stranded Together launcher core: the update check against GitHub Releases,
// the download, the verification of the release zip and the self-update (all
// or nothing, rolled back on any failure). Adapted from Diamond Dogs
// Together's core_update, with these differences:
//  - WinHTTP is never imported: the launcher sits next to the mod's
//    winhttp.dll proxy, so System32's winhttp.dll is loaded explicitly
//    (LoadLibraryExW + LOAD_LIBRARY_SEARCH_SYSTEM32, its path checked) and its
//    functions are taken with GetProcAddress (WinHttpApi);
//  - the network is behind an HttpClient seam (tests pass a fake feed);
//  - the release zip is verified against its own package.json (every file's
//    SHA-256, nothing unlisted, only the launcher, the mod and
//    stranded_together\ files), and against the feed's asset digest/size;
//  - prereleases are skipped unless asked for; drafts always.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core_paths.hpp"
#include "launcher_model.hpp"

namespace st::core {

// The project's releases (newest first; a page holds enough to skip drafts and prereleases).
inline constexpr char kReleasesUrl[] = "https://api.github.com/repos/TrollTaylor/stranded-together/releases?per_page=20";
// Where a release asset of the project downloads from (browser_download_url).
inline constexpr char kReleaseDownloadPrefix[] = "https://github.com/TrollTaylor/stranded-together/releases/download/";
// A release's page (html_url); anything else in a feed is dropped.
inline constexpr char kReleasePagePrefix[] = "https://github.com/TrollTaylor/stranded-together/releases/";
inline constexpr std::uint64_t kMaxFeedBytes = 4u << 20;
inline constexpr std::uint64_t kMaxUpdateZipBytes = 256ull << 20;

// ---- Versions ---------------------------------------------------------------------
// SemVer 2.0: "1.2.3", "v1.2.3", "1.2.3-rc.1", "1.2.3+abc1234" (build metadata is ignored).
struct SemVer {
    std::uint32_t major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre;   // dot-separated prerelease identifiers ("rc", "1")
};
bool ParseSemVer(const std::string& text, SemVer& out);
// <0, 0, >0 by SemVer precedence (a prerelease is lower than its release; numeric
// identifiers compare as numbers and below alphanumeric ones).
int CompareSemVer(const SemVer& a, const SemVer& b);
// candidate > current; false when either does not parse.
bool IsVersionNewer(const std::string& candidate, const std::string& current);
std::string FormatSemVer(const SemVer& v);   // "1.2.3" or "1.2.3-rc.1"
// The release zip of a version: StrandedTogether-<version>.zip.
std::string UpdateAssetName(const std::string& version);

// ---- The release feed -------------------------------------------------------------
struct FeedOptions {
    bool allow_prerelease = false;   // default: only published, non-prerelease releases
    // false: every download URL is kReleaseDownloadPrefix<tag>/<asset> (GitHub).
    // true (--update-feed): every download URL is a loopback URL (IsLoopbackUrl).
    bool local_feed = false;
};
// Reads GET /repos/.../releases (an array) or /releases/latest (one object) and
// picks the highest release (SemVer of tag_name) that is not a draft, not a
// prerelease (unless allowed), and carries exactly StrandedTogether-<version>.zip
// as an asset (never a source zip: zipball_url / tarball_url and any other asset
// are ignored). out.status = available when it is newer than current_version,
// else up_to_date. False (with `error`) only for a feed that is not JSON of that shape.
bool ParseReleasesFeed(const std::string& json_text, const std::string& current_version, UpdateInfo& out, std::string& error,
                       const FeedOptions& options = {});
// A --update-feed URL: http(s)://127.0.0.1[:port]/... or http(s)://localhost[:port]/... only.
bool IsLoopbackUrl(const std::string& url);

// ---- HTTP ---------------------------------------------------------------------------
struct HttpProgress {
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;   // 0 = unknown
};
class HttpClient {
public:
    virtual ~HttpClient() = default;
    // A whole body of at most max_bytes; status 200 only (redirects are followed).
    virtual bool Get(const std::string& url, std::uint64_t max_bytes, std::string& body, std::string& error) = 0;
    // Streams to dest_path (created or replaced); stops when *cancel turns true.
    virtual bool Download(const std::string& url, const std::wstring& dest_path, std::uint64_t max_bytes,
                          const std::function<void(const HttpProgress&)>& on_progress, const std::atomic<bool>* cancel,
                          std::string& error) = 0;
};
// The real client on System32's winhttp.dll (loaded on first use, never imported).
std::shared_ptr<HttpClient> MakeWinHttpClient(const std::string& user_agent);

// System32's winhttp.dll, loaded with LOAD_LIBRARY_SEARCH_SYSTEM32 and checked to
// be System32's file (a winhttp.dll from the game folder is the mod's proxy and
// must never be loaded into the launcher). `loaded_from` gets the module's path.
bool LoadSystemWinHttp(std::wstring* loaded_from, std::string* error);

// ---- Verifying a downloaded zip -------------------------------------------------------
// A Stranded Together release zip of `expected_version`:
//  - it opens (ZipReader: safe paths, sizes, CRCs), at most 64 entries;
//  - every entry is StrandedTogether.exe, winhttp.dll or under stranded_together/,
//    and none is a file the launcher or the mod writes (session.ini, settings, logs...);
//  - StrandedTogether.exe, winhttp.dll and stranded_together/package.json are there;
//  - package.json is ours (product, v 1), its version is expected_version, and its
//    files[] lists every other file of the zip exactly once with a matching SHA-256
//    (StrandedTogether.exe and winhttp.dll included);
//  - winhttp.dll is the Stranded Together proxy (LooksLikeOurDllBytes), the exe a PE.
struct ZipCheck {
    std::string version, build_id;
    std::vector<std::string> files;   // the zip's file entries (package.json included)
};
bool VerifyUpdateZip(const std::wstring& zip_path, const std::string& expected_version, ZipCheck& out, std::string& error);

// ---- Installing it --------------------------------------------------------------------
// All or nothing, over the game folder:
//  1. refused while ds.exe runs from the folder (game_running), when winhttp.dll in the
//     folder is another program's (never replaced), or when the zip does not verify;
//  2. every file is written next to its target as <target>.st-new;
//  3. each target is moved aside (the running StrandedTogether.exe to
//     StrandedTogether.exe.old, others to <target>.st-old) and the new file moved in;
//     a move that an antivirus holds for a moment is retried for about two seconds;
//  4. start_new (optional: the restart into the new launcher) runs while the old files
//     can still come back;
//  5. the .st-old files are deleted; StrandedTogether.exe.old is deleted by the new
//     launcher when it starts (CleanOldExecutable).
// Any failure in 2-4 moves every old file back (report.restored says whether all came
// back). A mod switched off (only winhttp.dll.disabled) stays switched off: the new DLL
// goes to winhttp.dll.disabled. The zip is deleted after a success.
enum class ApplyFailure {
    none,
    damaged,          // the zip does not verify (VerifyUpdateZip)
    game_running,     // ds.exe runs from the folder
    foreign_dll,      // winhttp.dll in the folder is not ours
    old_exe_in_use,   // StrandedTogether.exe.old of an earlier update cannot be deleted
    install_failed,   // a file could not be written or replaced
    restart_failed,   // start_new returned false
};
const char* ApplyFailureName(ApplyFailure failure);
struct ApplyReport {
    std::size_t files = 0;
    ApplyFailure failure = ApplyFailure::none;
    bool restored = true;   // after a failure: every old file is back in place
    std::string version;    // the zip's package.json version
};
struct ApplyOptions {
    std::function<bool()> game_running;                        // true: refuse (checked first and right before committing)
    std::function<bool(std::string& error)> start_new;         // the restart; false rolls everything back
    // Test seam: called before each file is committed (its zip name); false fails that move.
    std::function<bool(const std::string& name)> before_commit;
};
bool ApplyUpdate(const InstancePaths& paths, const std::wstring& zip_path, const std::string& expected_version,
                 const ApplyOptions& options, ApplyReport& report, std::string& error);
// The player's sentence for a failed ApplyUpdate.
std::string ApplyFailureSentence(const ApplyReport& report);

// What an update left behind when its process was ended half way, in the game folder
// and in stranded_together\: <file>.st-new is deleted; <file>.st-old is deleted when
// <file> exists, else moved back. Returns how many were handled.
std::size_t CleanUpdateLeftovers(const InstancePaths& paths);
// Deletes StrandedTogether.exe.old (retrying for up to wait_ms while the old
// launcher is still exiting); true when none is left.
bool CleanOldExecutable(const InstancePaths& paths, int wait_ms = 0);

// ---- Restart after an update ------------------------------------------------------------
// The launcher that installed the update (A) hands over to the new exe (B):
//  A  creates the event UpdateHandoffEventName(A's pid), starts B with
//     --after-update <A's pid> and waits until B sets the event (B runs) or B exits
//     (B failed: the update is rolled back). A B that does not answer within
//     ready_wait_ms is ended and the update rolled back too (a hung or crashed new
//     exe must not be left as the only launcher). Then A closes its window and exits.
//  B  sets the event, waits up to kPreviousLauncherWaitMs for A to exit (a hung A is
//     ended), then claims the folder's single instance, deletes the .old exe and says
//     "Updated to <version>".
inline constexpr int kSuccessorReadyWaitMs = 20000;
inline constexpr int kPreviousLauncherWaitMs = 15000;
inline constexpr int kStopPreviousWaitMs = 2000;
std::wstring UpdateHandoffEventName(std::uint32_t pid);
std::wstring QuoteArgument(const std::wstring& arg);
// A's side: starts exe with args in dir and waits for the handoff. False: B could not
// start, exited first, or did not answer in time and was ended (`error` says why).
bool StartSuccessor(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& dir, int ready_wait_ms,
                    std::string& error);
// B's side, in steps so the order is testable.
struct PreviousLauncherOps {
    std::function<bool()> signal;            // false: no launcher waits for us
    std::function<bool(int ms)> wait_exit;   // true: A has exited
    std::function<bool()> stop;              // ends a hung A
};
enum class TakeOver { none, exited, stopped };
TakeOver TakeOverFromPreviousLauncher(const PreviousLauncherOps& ops, int wait_ms);
PreviousLauncherOps PreviousLauncherWin32(std::uint32_t pid);

}  // namespace st::core
