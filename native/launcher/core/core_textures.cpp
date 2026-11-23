// Stranded Together launcher core: Fragile's textures (core_textures.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <sstream>

#include "core_install.hpp"
#include "core_paths.hpp"
#include "core_textures.hpp"
#include "extract_plan.hpp"
#include "tex_slots.hpp"

namespace st::core {

std::wstring SelfExePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

namespace {
std::wstring KnownFolder(REFKNOWNFOLDERID id, DWORD flags) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, flags, nullptr, &p)) && p && *p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}
bool IsLocalDrivePath(const std::wstring& p) { return p.size() >= 4 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'); }
// Documents when it is a local folder OneDrive does not sync, else "".
std::wstring LocalDocuments() {
    const std::wstring docs = KnownFolder(FOLDERID_Documents, KF_FLAG_DEFAULT);
    return !docs.empty() && IsLocalDrivePath(docs) && !InOneDrive(docs) ? docs : std::wstring();
}
constexpr wchar_t kTexturesSub[] = L"\\Stranded Together\\Textures";
}  // namespace

bool InOneDrive(const std::wstring& folder) {
    if (folder.empty()) return false;
    for (const wchar_t* var : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
        wchar_t buf[MAX_PATH + 1] = {};
        const DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH + 1);
        if (n > 3 && n <= MAX_PATH && tex::PathInside(folder, buf)) return true;
    }
    const std::wstring sky = KnownFolder(FOLDERID_SkyDrive, KF_FLAG_DONT_VERIFY);
    return !sky.empty() && tex::PathInside(folder, sky);
}

std::wstring DefaultTexturesDir() {
    const std::wstring docs = LocalDocuments();
    if (!docs.empty()) return docs + kTexturesSub;
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData, KF_FLAG_DEFAULT);
    if (!local.empty() && IsLocalDrivePath(local)) return local + kTexturesSub;
    return {};
}

std::wstring TexturesDirFromSettings(const json::Value& settings) {
    if (const json::Value* t = settings.Find("textures"); t && t->is_object()) {
        const std::string dir = t->Str("dir");
        if (!dir.empty()) return tex::Widen(dir);
    }
    return DefaultTexturesDir();
}

std::string TexturesDirForSession(const json::Value& settings, const std::wstring& game_dir, std::string* why) {
    const std::wstring dir = TexturesDirFromSettings(settings);
    if (dir.empty()) return {};
    std::string problem = TexturesFolderProblem(dir, game_dir);
    const std::string u = tex::Narrow(dir);
    if (problem.empty() && u.size() > 1024) problem = "The textures folder's path is too long for the mod.";
    if (!problem.empty()) {
        if (why) *why = problem;
        return {};
    }
    return u;
}

std::string TexturesFolderNote(const std::wstring& folder, bool custom, const std::wstring& game_dir) {
    if (folder.empty()) return {};
    const std::string problem = TexturesFolderProblem(folder, game_dir);
    if (!problem.empty()) return "The mod cannot use this folder (sessions start without textures): " + problem;
    if (InOneDrive(folder))
        return "This folder is synced by OneDrive: the extracted game pictures (about 330 MB) and your edits would be uploaded. "
               "Pick a folder outside OneDrive with Change folder...";
    if (!custom && LocalDocuments().empty())
        return "Your Documents folder is synced by OneDrive (or not on this PC), so the textures go to this local folder instead.";
    return {};
}

std::string BootsLookFromSettings(const json::Value& settings) {
    if (const json::Value* t = settings.Find("textures"); t && t->is_object() && t->Str("boots") == "prop") return "prop";
    return "own";
}

bool TexturePreviewsFromSettings(const json::Value& settings) {
    if (const json::Value* t = settings.Find("textures"); t && t->is_object()) return t->Bool("previews", true);
    return true;
}

bool ReadyMadeFromSettings(const json::Value& settings) {
    if (const json::Value* t = settings.Find("textures"); t && t->is_object()) return t->Bool("defaults", true);
    return true;
}

bool AutoExtractFromSettings(const json::Value& settings) {
    if (const json::Value* t = settings.Find("textures"); t && t->is_object()) return t->Bool("auto_extract", true);
    return true;
}

ManifestFacts ReadManifestFacts(const std::wstring& dir) {
    ManifestFacts f;
    if (dir.empty()) return f;
    const std::wstring path = dir + L"\\" + tex::Widen(tex::kManifestName);
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        // a file that exists but cannot be opened right now (the extractor replacing it, an antivirus scan) still counts
        // as there, and as nothing known yet (DecideAutoExtract looks again: never an extraction on it)
        f.present = e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND;
        f.open_failed = f.present;
        return f;
    }
    f.present = true;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16ll << 20)) {
        text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(h);
    tex::Manifest m;
    std::string err;
    if (text.empty() || !tex::ParseManifest(text, m, err)) return f;
    f.readable = true;
    f.extract = m.extract;
    f.table_ok = m.table == tex::kTableVersion;
    return f;
}

namespace {
std::string LowerUtf8(const std::wstring& dir) {
    std::wstring d = dir;
    while (d.size() > 3 && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    for (wchar_t& c : d) c = static_cast<wchar_t>(towlower(c));
    return tex::Narrow(d);
}
}  // namespace

bool WasExtractedHere(const json::Value& settings, const std::wstring& dir) {
    const json::Value* t = settings.Find("textures");
    return !dir.empty() && t && t->is_object() && t->Str("extracted") == LowerUtf8(dir);
}

bool RememberExtractedHere(json::Value& settings, const std::wstring& dir) {
    if (dir.empty() || WasExtractedHere(settings, dir)) return false;
    json::Value t = json::Value::Object();
    if (const json::Value* old = settings.Find("textures"); old && old->is_object()) t = *old;
    t["extracted"] = LowerUtf8(dir);
    settings["textures"] = t;
    return true;
}

std::uint64_t FreeBytesFor(const std::wstring& dir) {
    std::wstring probe = dir;
    while (probe.size() > 3) {
        const DWORD a = GetFileAttributesW(probe.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) break;
        const std::size_t slash = probe.find_last_of(L"\\/");
        if (slash == std::wstring::npos || slash < 2) break;
        probe.resize(slash < 3 ? 3 : slash);
    }
    if (probe.size() == 2) probe += L"\\";
    ULARGE_INTEGER avail{};
    if (probe.empty() || !GetDiskFreeSpaceExW(probe.c_str(), &avail, nullptr, nullptr)) return UINT64_MAX;
    return avail.QuadPart;
}

std::string AutoExtractKey(const std::wstring& dir, const std::string& game_stamp) {
    std::wstring d = dir;
    for (wchar_t& c : d) c = static_cast<wchar_t>(towlower(c));
    // this build's version too: a Stranded Together update may fix what failed, so it starts from zero
#ifdef ST_LAUNCHER_VERSION
    const std::string version = std::string(ST_LAUNCHER_VERSION) + "|";
#else
    const std::string version;
#endif
    return version + std::to_string(tex::kExtractVersion) + "|" + game_stamp + "|" + tex::Narrow(d);
}

AutoExtractRecord AutoExtractRecordFromSettings(const json::Value& settings) {
    AutoExtractRecord r;
    const json::Value* t = settings.Find("textures");
    const json::Value* a = t && t->is_object() ? t->Find("auto") : nullptr;
    if (!a || !a->is_object()) return r;
    r.key = a->Str("key");
    r.failures = static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(a->Int("failures", 0), 1000)));
    r.last_s = a->Int("last_s", 0);
    r.error = a->Str("error");
    return r;
}

void StoreAutoExtractRecord(json::Value& settings, const AutoExtractRecord& r) {
    json::Value t = json::Value::Object();
    if (const json::Value* old = settings.Find("textures"); old && old->is_object()) t = *old;
    if (r.key.empty()) {
        if (!t.Remove("auto")) return;
    } else {
        json::Value a = json::Value::Object();
        a["key"] = r.key;
        a["failures"] = static_cast<std::int64_t>(r.failures);
        a["last_s"] = r.last_s;
        a["error"] = r.error.size() > 300 ? r.error.substr(0, 300) : r.error;
        t["auto"] = a;
    }
    settings["textures"] = t;
}

std::int64_t AutoExtractRetryAfterS(int failures) {
    if (failures <= 0) return 0;
    if (failures == 1) return 10 * 60;
    if (failures == 2) return 60 * 60;
    return -1;
}

bool AutoExtractWillRun(AutoExtract what) {
    return what == AutoExtract::start || what == AutoExtract::busy || what == AutoExtract::wait_game || what == AutoExtract::retry_later ||
           what == AutoExtract::look_again;
}

AutoExtractDecision DecideAutoExtract(const AutoExtractFacts& f) {
    AutoExtractDecision d;
    const ManifestFacts& m = f.manifest;
    if (m.present && m.readable && m.table_ok && m.extract >= tex::kExtractVersion) return d;   // this version's (or newer)
    if (!m.present) {
        d.reason = f.was_extracted ? "its extraction is gone (no .manifest.txt)" : "never extracted";
        d.user_reason = f.was_extracted ? "the extracted textures are no longer in the folder" : "not extracted yet";
    } else if (!m.readable) {
        d.reason = m.open_failed ? "its index (.manifest.txt) cannot be opened right now" : "its index (.manifest.txt) does not read";
        d.user_reason = "the folder's index does not read";
    } else if (!m.table_ok) {
        d.reason = "extracted by another Stranded Together (another texture table)";
        d.user_reason = "Stranded Together was updated";
    } else {
        d.reason = "extracted by an older Stranded Together (extract " + std::to_string(m.extract) + ", now " + std::to_string(tex::kExtractVersion) + ")";
        d.user_reason = "Stranded Together was updated";
    }
    if (!f.enabled) return d.what = AutoExtract::off, d;
    if (!f.folder_usable) return d.what = AutoExtract::no_folder, d;
    // never on what is not known: an index another program holds is looked at again; one that does not read (damaged, or
    // a newer Stranded Together's) and a folder that lost its extraction are the player's call (an extraction there has
    // no record of which pictures are unpainted, or fills a drive the player emptied)
    if (m.present && !m.readable) return d.what = m.open_failed ? AutoExtract::look_again : AutoExtract::unreadable, d;
    if (!m.present && f.was_extracted) return d.what = AutoExtract::gone, d;
    if (f.onedrive) return d.what = AutoExtract::onedrive, d;
    if (f.extracting) return d.what = AutoExtract::busy, d;
    if (f.cancelled) return d.what = AutoExtract::cancelled, d;
    if (f.failures >= kAutoExtractMaxFailures) return d.what = AutoExtract::gave_up, d;
    if (f.failures > 0) {
        const std::int64_t after = AutoExtractRetryAfterS(f.failures);
        // a clock that went backwards (last in the future) never blocks it for longer than the wait itself
        const std::int64_t last = std::min(f.last_failure_s, f.now_s);
        if (after < 0) return d.what = AutoExtract::gave_up, d;
        if (f.now_s < last + after) {
            d.retry_at_s = last + after;
            return d.what = AutoExtract::retry_later, d;
        }
    }
    if (f.low_disk) return d.what = AutoExtract::low_disk, d;
    if (f.game_running) return d.what = AutoExtract::wait_game, d;
    d.what = AutoExtract::start;
    return d;
}

std::string TexturesFolderProblem(const std::wstring& folder, const std::wstring& game_dir) {
    if (folder.size() < 4 || folder[1] != L':' || (folder[2] != L'\\' && folder[2] != L'/'))
        return "Pick a folder on a drive (like C:\\...), not a network or relative path.";
    for (wchar_t c : folder)
        if (c < 32) return "That folder name has characters the mod cannot read.";
    if (!game_dir.empty() && tex::PathInside(folder, game_dir)) return "Pick a folder outside the game folder (the game's files are never touched).";
    wchar_t win[MAX_PATH] = {};
    if (GetWindowsDirectoryW(win, MAX_PATH) && tex::PathInside(folder, win)) return "Pick a folder outside Windows' own folder.";
    for (REFKNOWNFOLDERID id : {FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) {
            const bool inside = tex::PathInside(folder, p);
            CoTaskMemFree(p);
            if (inside) return "Pick a folder outside Program Files (Windows protects it).";
        } else if (p) {
            CoTaskMemFree(p);
        }
    }
    return {};
}

std::string AutomationOutProblem(const std::wstring& out) {
    // Never the user's own folders: Documents, Desktop, Pictures and OneDrive wherever they are, and nothing in the
    // user profile except the temp folder (a test's scratch space). The testenv's run folders live in the repo.
    for (REFKNOWNFOLDERID id : {FOLDERID_Documents, FOLDERID_Desktop, FOLDERID_Pictures, FOLDERID_SkyDrive}) {
        PWSTR p = nullptr;
        const bool got = SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p && *p;
        const bool inside = got && tex::PathInside(out, p);
        if (p) CoTaskMemFree(p);
        if (inside) return "--allow-out-under is for test folders: refusing a folder inside the user's Documents, Desktop, Pictures or OneDrive.";
    }
    PWSTR profile = nullptr;
    bool in_profile = false;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DONT_VERIFY, nullptr, &profile)) && profile && *profile)
        in_profile = tex::PathInside(out, profile);
    if (profile) CoTaskMemFree(profile);
    if (in_profile) {
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
        if (n == 0 || n > MAX_PATH || !tex::PathInside(out, temp))
            return "--allow-out-under is for test folders: inside the user profile only the temp folder is allowed.";
    }
    return {};
}

std::string FormatMegabytes(std::uint64_t bytes) {
    const std::uint64_t mb = (bytes + (1u << 19)) >> 20;
    return std::to_string(mb) + " MB";
}

void ApplyExtractLine(const std::string& raw, ExtractProgress& p) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    auto word = [&](const char* w) { return line.rfind(w, 0) == 0; };
    if (word("progress ")) {
        std::istringstream in(line.substr(9));
        int d = 0, t = 0;
        in >> d >> t;
        std::string rest;
        std::getline(in, rest);
        if (!rest.empty() && rest[0] == ' ') rest.erase(0, 1);
        if (t > 0 && d >= 0 && d <= t) {
            p.done = d;
            p.total = t;
            p.current = rest;
        }
    } else if (word("set ")) {
        ++p.sets;
    } else if (word("skip ")) {
        ++p.skipped;
    } else if (word("kept ")) {
        ++p.kept;
    } else if (word("note ")) {
        // every note counts (an "outdated seeds" note after a "not made" one must not hide it)
        const std::string n = line.substr(5);
        p.note += (p.note.empty() ? "" : ". ") + n;
        if (n.rfind("ready-made colours not made", 0) == 0) p.defaults_not_made = true;
    } else if (word("incomplete ")) {
        std::istringstream in(line.substr(11));
        int n = 0;
        if (in >> n && n > 0) p.incomplete = n;
    } else if (word("defaults ")) {
        std::istringstream in(line.substr(9));
        int w = 0, r = 0, y = 0;
        unsigned long long ms = 0;
        if (in >> w >> r >> y >> ms) {
            p.defaults_seen = true;
            p.defaults_written = w;
            p.defaults_replaced = r;
            p.folders_yours = y;
            p.defaults_ms = ms;
        }
    } else if (word("error ")) {
        p.error = line.substr(6);
    } else if (word("done ")) {
        std::istringstream in(line.substr(5));
        int written = 0, kept = 0;
        unsigned long long bytes = 0, ms = 0;
        if (in >> written >> kept >> bytes >> ms) {
            p.written = written;
            p.kept = kept;
            p.bytes = bytes;
            p.ms = ms;
            p.finished = true;
            if (p.total > 0) p.done = p.total;
        }
    }
}

std::wstring ExtractCommandLine(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews,
                                const std::wstring& allow_under) {
    auto quote = [](const std::wstring& s) {
        std::wstring q = L"\"" + s;
        // A trailing backslash would escape the closing quote.
        while (!q.empty() && q.back() == L'\\') q.pop_back();
        return q + L"\"";
    };
    std::wstring c = quote(exe) + L" --extract-textures --game-dir " + quote(game_dir) + L" --out " + quote(out_dir);
    if (!previews) c += L" --no-previews";
    if (!allow_under.empty()) c += L" --allow-out-under " + quote(allow_under);
    return c;
}

int RunExtractTextures(const ExtractRequest& r, const std::function<void(const std::string&)>& print) {
    auto fail = [&](const std::string& why) {
        print("error " + why);
        return 1;
    };
    if (r.out_dir.empty()) return fail("--out <folder> is needed.");
    if (r.out_dir.size() < 4 || r.out_dir[1] != L':') return fail("--out must be a full path.");
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoGuard {
        HRESULT hr;
        ~CoGuard() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } guard{co};
    if (r.check_only) {
        tex::EditScan s;
        tex::ScanEdits(r.out_dir, true, s);
        std::istringstream in(tex::CheckReport(s));
        std::string line;
        while (std::getline(in, line)) print("check " + line);
        return 0;
    }
    const std::wstring game = r.game_dir;
    if (game.empty() || GetFileAttributesW((game + L"\\ds.exe").c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail("No ds.exe in the game folder: run this from the DEATH STRANDING DIRECTOR'S CUT folder.");
    if (!r.allow_under.empty()) {
        // Automation (the hidden check, tests): only inside the given folder, never inside the user's Documents.
        if (!tex::PathInside(r.out_dir, r.allow_under)) return fail("--out is not inside --allow-out-under.");
        const std::string why = AutomationOutProblem(r.out_dir);
        if (!why.empty()) return fail(why);
    } else {
        json::Value settings;
        ReadSettings(GetInstancePaths(game), settings);
        const std::wstring chosen = TexturesDirFromSettings(settings);
        if (chosen.empty() || !tex::PathInside(r.out_dir, chosen) || !tex::PathInside(chosen, r.out_dir))
            return fail("--out is not the textures folder the launcher's settings name.");
        const std::string problem = TexturesFolderProblem(r.out_dir, game);
        if (!problem.empty()) return fail(problem);
    }
    tex::ExtractOptions o;
    o.game_dir = game;
    o.out_dir = r.out_dir;
    o.previews = r.previews;
    o.cancel_after_sets = r.cancel_after_sets;
#ifdef ST_LAUNCHER_VERSION
    o.tool_version = ST_LAUNCHER_VERSION;
#endif
    o.emit = print;
    tex::ExtractStats stats;
    std::string error;
    if (!tex::RunExtract(o, stats, error)) return fail(error);
    return 0;
}

// ---- TextureTasks ----------------------------------------------------------------------------------------------
TextureTasks::TextureTasks() = default;

TextureTasks::~TextureTasks() {
    Cancel();   // a window closing mid-extraction: the next run deletes the partial files
    if (reader_.joinable()) reader_.join();
    if (scanner_.joinable()) scanner_.join();
    if (process_) CloseHandle(static_cast<HANDLE>(process_));
}

bool TextureTasks::StartExtract(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews,
                                std::string& error, const std::wstring& allow_under) {
    if (extracting_) return error = "The textures are already being extracted.", false;
    if (reader_.joinable()) reader_.join();
    if (process_) {
        CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return error = "Could not start the extraction (pipe).", false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = ExtractCommandLine(exe, game_dir, out_dir, previews, allow_under);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si,
                        &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return error = "Could not start the extraction (error " + std::to_string(GetLastError()) + ").", false;
    }
    CloseHandle(wr);
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(mu_);
        process_ = pi.hProcess;
        progress_ = ExtractProgress{};
        exit_code_ = 0;
    }
    finished_ = false;
    extracting_ = true;
    reader_ = std::thread([this, rd] { ReaderLoop(rd); });
    return true;
}

void TextureTasks::ReaderLoop(void* pipe) {
    HANDLE rd = static_cast<HANDLE>(pipe);
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) {
        pending.append(buf, got);
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            std::lock_guard<std::mutex> lock(mu_);
            ApplyExtractLine(line, progress_);
        }
    }
    CloseHandle(rd);
    DWORD code = 1;
    {
        HANDLE p = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            p = static_cast<HANDLE>(process_);
        }
        if (p) {
            WaitForSingleObject(p, 10000);
            GetExitCodeProcess(p, &code);
        }
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!pending.empty()) ApplyExtractLine(pending, progress_);
        exit_code_ = static_cast<int>(code);
    }
    extracting_ = false;
    finished_ = true;
}

void TextureTasks::Cancel() {
    std::lock_guard<std::mutex> lock(mu_);
    if (extracting_ && process_) TerminateProcess(static_cast<HANDLE>(process_), 1);
}

bool TextureTasks::extracting() const { return extracting_; }

ExtractProgress TextureTasks::progress() const {
    std::lock_guard<std::mutex> lock(mu_);
    return progress_;
}

bool TextureTasks::TakeFinished(ExtractProgress& p, int& exit_code) {
    bool expected = true;
    if (!finished_.compare_exchange_strong(expected, false)) return false;
    std::lock_guard<std::mutex> lock(mu_);
    p = progress_;
    exit_code = exit_code_;
    return true;
}

void TextureTasks::StartScan(const std::wstring& dir, bool full, bool ready_made) {
    if (scanning_) return;
    if (scanner_.joinable()) scanner_.join();
    scanning_ = true;
    scanner_ = std::thread([this, dir, full, ready_made] {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ScanResult r;
        r.valid = true;
        r.full = full;
        if (!dir.empty()) {
            try {  // a picture that needs more memory than there is must never close the launcher
                tex::EditScan s;
                tex::ScanEdits(dir, full, s);
                r.extracted = s.extracted;
                r.table_ok = s.table_ok;
                r.extract_ok = s.manifest.extract >= tex::kExtractVersion;   // a newer one's too (DecideAutoExtract: not needed)
                r.extracted_at = s.extracted_utc;
                r.colour_folders = s.colour_folders;
                r.boot_folders = s.boot_folders;
                r.outdated_seeds = s.outdated_seeds;
                r.ready_made = s.ready_made;
                r.stale_ready_made = s.stale_ready_made;
                for (const tex::EditedFolder& ef : s.edited) {
                    const std::size_t slash = ef.folder.find('\\');
                    const int n = ef.folder.rfind(tex::kFragileSuitDir, 0) == 0 && slash != std::string::npos ? tex::FolderNumber(ef.folder.substr(slash + 1)) : -1;
                    if (n >= 1 && n <= 16) ++r.painted_colours;
                }
                r.problems = static_cast<int>(s.problems.size());
                r.edited = tex::EditSummary(s);
                r.report = tex::CheckReport(s, ready_made);
                r.game = s.manifest.game;
            } catch (...) {
                r = ScanResult{};
                r.valid = true;
                r.full = full;
                r.report = "The check stopped: the pictures need more memory than there is. Close other programs and try again.";
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
        {
            std::lock_guard<std::mutex> lock(mu_);
            scan_ = r;
        }
        scanning_ = false;
    });
}

bool TextureTasks::scanning() const { return scanning_; }

TextureTasks::ScanResult TextureTasks::scan() const {
    std::lock_guard<std::mutex> lock(mu_);
    return scan_;
}

}  // namespace st::core
