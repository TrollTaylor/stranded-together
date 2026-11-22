// Stranded Together launcher core: Fragile's textures (core_textures.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdlib>
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
        p.note = line.substr(5);
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

std::wstring ExtractCommandLine(const std::wstring& exe, const std::wstring& game_dir, const std::wstring& out_dir, bool previews) {
    auto quote = [](const std::wstring& s) {
        std::wstring q = L"\"" + s;
        // A trailing backslash would escape the closing quote.
        while (!q.empty() && q.back() == L'\\') q.pop_back();
        return q + L"\"";
    };
    std::wstring c = quote(exe) + L" --extract-textures --game-dir " + quote(game_dir) + L" --out " + quote(out_dir);
    if (!previews) c += L" --no-previews";
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
                                std::string& error) {
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
    std::wstring cmd = ExtractCommandLine(exe, game_dir, out_dir, previews);
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

void TextureTasks::StartScan(const std::wstring& dir, bool full) {
    if (scanning_) return;
    if (scanner_.joinable()) scanner_.join();
    scanning_ = true;
    scanner_ = std::thread([this, dir, full] {
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
                r.extract_ok = s.manifest.extract == tex::kExtractVersion;
                r.extracted_at = s.extracted_utc;
                r.colour_folders = s.colour_folders;
                r.boot_folders = s.boot_folders;
                r.outdated_seeds = s.outdated_seeds;
                r.problems = static_cast<int>(s.problems.size());
                r.edited = tex::EditSummary(s);
                r.report = tex::CheckReport(s);
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
