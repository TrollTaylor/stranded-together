#include "testenv_core.hpp"

#include <bcrypt.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <regex>
#include <sstream>

#include "automation.hpp"

namespace st::testenv {

// ------------------------------------------------------------------ text / files
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (u < 0x20) {
            char b[8];
            std::snprintf(b, sizeof(b), "\\u%04x", u);
            o += b;
        } else o += c;
    }
    return o;
}

bool ReadFileBytes(const std::wstring& path, std::string* out, std::uint64_t max_bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && static_cast<std::uint64_t>(size.QuadPart) <= max_bytes;
    if (ok) {
        out->assign(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t done = 0;
        while (ok && done < out->size()) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out->size() - done, 1u << 24));
            ok = ReadFile(f, out->data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(f);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& bytes) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty() || DirExists(path)) return true;
    const auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) EnsureDir(path.substr(0, slash));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool RemoveTree(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path.c_str()) != FALSE : DeleteFileW(path.c_str()) != FALSE;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            RemoveTree(path + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(path.c_str()) != FALSE;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[4096];
    const DWORD n = GetFullPathNameW(p.c_str(), 4096, buf, nullptr);
    if (n == 0 || n >= 4096) return p;
    std::wstring s(buf, n);
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

std::string Sha256File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
            ok = BCryptHashData(hash, buf.data(), got, 0) == 0;
        unsigned char digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            char b[3];
            for (unsigned char c : digest) {
                std::snprintf(b, sizeof(b), "%02x", c);
                hex += b;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

namespace {
void ListInto(const std::wstring& root, const std::wstring& rel, bool recursive, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (recursive) ListInto(root, r, true, out);
        } else {
            out.push_back(r);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace

std::vector<std::wstring> ListFiles(const std::wstring& root, bool recursive) {
    std::vector<std::wstring> out;
    ListInto(root, L"", recursive, out);
    std::sort(out.begin(), out.end());
    return out;
}

bool CopyTree(const std::wstring& from, const std::wstring& to) {
    bool ok = EnsureDir(to);
    for (const std::wstring& rel : ListFiles(from, true)) {
        const std::wstring dst = to + L"\\" + rel;
        const auto slash = dst.find_last_of(L'\\');
        EnsureDir(dst.substr(0, slash));
        ok = CopyFileW((from + L"\\" + rel).c_str(), dst.c_str(), FALSE) && ok;
    }
    return ok;
}

std::string NowStamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string IsoNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[40];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02uT%02u:%02u:%02u.%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                  t.wMilliseconds);
    return b;
}

// ------------------------------------------------------------------ session.ini
std::string RedirectOnlyIniText(const std::wstring& save_root) {
    std::string s =
        "# Generated by Stranded Together testenv (hidden test run, --no-mod): no session, the mod only\n"
        "# redirects the saves. The supervisor restores the previous file afterwards.\n";
    s += "one_shot=1\nautomation=1\n";
    if (!save_root.empty()) s += "automation_save_root=" + Narrow(save_root) + "\n";
    return s;
}

std::string SessionIniText(const std::string& role, bool mute, bool isolate, const std::wstring& save_root) {
    std::string s =
        "# Generated by Stranded Together testenv (hidden test run). The supervisor restores the previous file afterwards.\n";
    s += "role=" + role + "\n";
    s += "transport=udp\nudp_peer=127.0.0.1\nudp_port_local=27555\nudp_port_peer=27556\n";
    s += "debug_menu=1\ndebug_menu_key=F9\nverbose=1\none_shot=1\n";
    s += "automation=1\n";
    s += std::string("automation_mute=") + (mute ? "1" : "0") + "\n";
    if (!isolate) s += "automation_isolate=0\n";
    if (!save_root.empty()) s += "automation_save_root=" + Narrow(save_root) + "\n";
    return s;
}

// ------------------------------------------------------------------ scenarios
namespace {
std::string Trim(const std::string& s) { return automation::detail::Trim(s); }
bool ParseU(const std::string& s, std::uint32_t lo, std::uint32_t hi, std::uint32_t* v) {
    return automation::detail::Uint(s, lo, hi, v);
}
}  // namespace

bool ParseScenario(const std::string& text, Scenario* out, std::string* error) {
    *out = Scenario{};
    std::istringstream in(text);
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        if (lineno == 1 && raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF) raw = raw.substr(3);
        std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;
        Step st;
        st.line = lineno;
        if (line[0] == '?') {
            st.optional = true;
            line = Trim(line.substr(1));
        }
        const auto words = automation::detail::Split(line);
        const std::string verb = automation::detail::Lower(words[0]);
        const auto fail = [&](const std::string& why) {
            *error = "line " + std::to_string(lineno) + ": " + why;
            return false;
        };
        if (verb == "role") {
            if (words.size() != 2 || (words[1] != "solo" && words[1] != "echo")) return fail("role solo|echo");
            out->role = words[1];
            continue;
        }
        if (verb == "timeout") {
            std::uint32_t s = 0;
            if (words.size() != 2 || !ParseU(words[1], 30, 360, &s)) return fail("timeout <30..360 s>");
            out->timeout_s = s;
            continue;
        }
        if (verb == "waitready") {
            if (words.size() != 2 || !ParseU(words[1], 1, 360, &st.seconds)) return fail("waitready <s>");
            st.kind = StepKind::waitready;
        } else if (verb == "waitlog") {
            if (words.size() < 3 || !ParseU(words[1], 1, 360, &st.seconds)) return fail("waitlog <s> <regex>");
            st.kind = StepKind::waitlog;
            st.text = automation::detail::Rest(line, 2);
            try {
                std::regex re(st.text);
            } catch (const std::regex_error&) {
                return fail("waitlog: bad regex '" + st.text + "'");
            }
        } else if (verb == "until") {
            const auto arrow = line.find("=>");
            if (arrow == std::string::npos) return fail("until <s> <every_ms> <regex> => <mod command>");
            const auto head = automation::detail::Split(line.substr(0, arrow));
            if (head.size() < 4 || !ParseU(head[1], 1, 360, &st.seconds) || !ParseU(head[2], 100, 60000, &st.ms))
                return fail("until <s> <every_ms 100..60000> <regex> => <mod command>");
            st.text = Trim(automation::detail::Rest(line.substr(0, arrow), 3));
            st.command = Trim(line.substr(arrow + 2));
            try {
                std::regex re(st.text);
            } catch (const std::regex_error&) {
                return fail("until: bad regex '" + st.text + "'");
            }
            automation::Command c;
            std::string err;
            if (!automation::ParseCommand(st.command, &c, &err)) return fail("until: " + (err.empty() ? "no command" : err));
            if (c.kind == automation::Kind::quit) return fail("until: quit cannot repeat");
            st.kind = StepKind::until;
        } else if (verb == "sleep") {
            if (words.size() != 2 || !ParseU(words[1], 0, 300000, &st.ms)) return fail("sleep <ms>");
            st.kind = StepKind::sleep;
        } else if (verb == "note") {
            st.kind = StepKind::note;
            st.text = automation::detail::Rest(line, 1);
        } else {
            automation::Command c;
            std::string err;
            if (!automation::ParseCommand(line, &c, &err)) return fail(err.empty() ? "empty command" : err);
            st.kind = StepKind::mod;
            st.text = line;
        }
        out->steps.push_back(st);
    }
    if (out->steps.empty()) {
        *error = "the scenario has no steps";
        return false;
    }
    return true;
}

unsigned AckTimeoutSeconds(const std::string& mod_command) {
    automation::Command c;
    std::string err;
    if (!automation::ParseCommand(mod_command, &c, &err)) return 15;
    switch (c.kind) {
        case automation::Kind::wait:
        case automation::Kind::pad:
        case automation::Kind::key: return c.ms / 1000 + 15;
        case automation::Kind::waitplayer:
        case automation::Kind::waitlog: return c.ms / 1000 + 15;
        case automation::Kind::screenshot: return 40;
        case automation::Kind::action: return 30;
        case automation::Kind::quit: return 30;
        default: return 15;
    }
}

// ------------------------------------------------------------------ game folder guard
namespace {

// restore.txt lines (tab separated, paths UTF-8):
//   game <game dir>
//   file <existed 0|1> <abs path> <backup path or -> <sha256 or ->
//   tree <abs path>                         removed on restore (ours)
//   watch <abs dir>                         files that appear here are collected to game-new
//   absent <abs path>                       must not exist after the run (checked, removed)
struct Entry {
    std::string kind;
    bool existed = false;
    std::wstring path, backup;
    std::string sha;
};

std::vector<Entry> ReadManifest(const std::wstring& run_dir, std::wstring* game_dir) {
    std::string text;
    std::vector<Entry> out;
    if (!ReadFileBytes(run_dir + L"\\restore.txt", &text)) return out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::size_t at = 0;
        while (at <= line.size()) {
            std::size_t tab = line.find('\t', at);
            if (tab == std::string::npos) tab = line.size();
            f.push_back(line.substr(at, tab - at));
            at = tab + 1;
        }
        if (f.size() >= 2 && f[0] == "game") *game_dir = Widen(f[1]);
        Entry e;
        e.kind = f[0];
        if (e.kind == "file" && f.size() >= 5) {
            e.existed = f[1] == "1";
            e.path = Widen(f[2]);
            e.backup = f[3] == "-" ? L"" : Widen(f[3]);
            e.sha = f[4] == "-" ? "" : f[4];
            out.push_back(e);
        } else if ((e.kind == "tree" || e.kind == "watch" || e.kind == "absent") && f.size() >= 2) {
            e.path = Widen(f[1]);
            out.push_back(e);
        }
    }
    return out;
}

bool Inside(const std::wstring& path, const std::wstring& dir) {
    const std::wstring p = FullPath(path), d = FullPath(dir);
    return p.size() > d.size() + 1 && _wcsnicmp(p.c_str(), d.c_str(), d.size()) == 0 && p[d.size()] == L'\\';
}

}  // namespace

bool BackupGameFolder(const std::wstring& game_dir_in, const std::wstring& run_dir, std::string* error) {
    const std::wstring game = FullPath(game_dir_in);
    const std::wstring mod = game + L"\\stranded_together";
    const std::wstring backup = run_dir + L"\\backup";
    if (FileExists(run_dir + L"\\restore.txt")) {
        *error = "restore.txt already exists in the run folder (a run folder is used once)";
        return false;
    }
    if (!EnsureDir(backup)) {
        *error = "cannot create " + Narrow(backup);
        return false;
    }
    std::vector<std::wstring> files = {game + L"\\winhttp.dll", game + L"\\winhttp.dll.disabled", game + L"\\error.txt"};
    for (const std::wstring& rel : ListFiles(mod, false)) files.push_back(mod + L"\\" + rel);
    for (const wchar_t* n : {L"session.ini", L"session.last.ini", L"stranded_together.log", L"link-status.json"}) {
        const std::wstring p = mod + L"\\" + n;
        if (std::find(files.begin(), files.end(), p) == files.end()) files.push_back(p);
    }
    std::string manifest = "game\t" + Narrow(game) + "\n";
    int n = 0;
    for (const std::wstring& p : files) {
        const bool existed = FileExists(p);
        std::string sha = "-";
        std::wstring copy;
        if (existed) {
            copy = backup + L"\\" + std::to_wstring(n++) + L"-" + p.substr(p.find_last_of(L'\\') + 1);
            if (!CopyFileW(p.c_str(), copy.c_str(), FALSE)) {
                *error = "cannot back up " + Narrow(p) + " (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            sha = Sha256File(copy);
            if (sha.empty() || sha != Sha256File(p)) {
                *error = "backup of " + Narrow(p) + " does not match the original";
                return false;
            }
        }
        manifest += "file\t" + std::string(existed ? "1" : "0") + "\t" + Narrow(p) + "\t" + (existed ? Narrow(copy) : "-") + "\t" +
                    sha + "\n";
    }
    manifest += "tree\t" + Narrow(mod + L"\\automation") + "\n";
    manifest += "watch\t" + Narrow(mod) + "\n";
    manifest += "absent\t" + Narrow(game + L"\\steam_appid.txt") + "\n";
    if (!DirExists(mod)) manifest += "tree-if-empty\t" + Narrow(mod) + "\n";
    if (!WriteFileAtomic(run_dir + L"\\restore.txt", manifest)) {
        *error = "cannot write restore.txt";
        return false;
    }
    return true;
}

GuardResult RestoreGameFolder(const std::wstring& run_dir) {
    GuardResult r;
    std::wstring game;
    const std::vector<Entry> entries = ReadManifest(run_dir, &game);
    if (game.empty() || entries.empty()) {
        r.notes.push_back("no restore.txt in the run folder: nothing to restore");
        return r;
    }
    bool ok = true;
    std::vector<std::wstring> known;
    for (const Entry& e : entries)
        if (e.kind == "file") known.push_back(FullPath(e.path));
    // 1. our trees (automation channel)
    for (const Entry& e : entries) {
        if (e.kind != "tree" || !Inside(e.path, game)) continue;
        if (DirExists(e.path)) {
            const bool gone = RemoveTree(e.path);
            ok = ok && gone;
            r.notes.push_back(std::string(gone ? "removed " : "COULD NOT remove ") + Narrow(e.path));
        }
    }
    // 2. new files in watched folders -> <run>\game-new
    for (const Entry& e : entries) {
        if (e.kind != "watch" || !Inside(e.path, game)) continue;
        for (const std::wstring& rel : ListFiles(e.path, false)) {
            const std::wstring p = FullPath(e.path + L"\\" + rel);
            bool is_known = false;
            for (const std::wstring& k : known) is_known = is_known || _wcsicmp(k.c_str(), p.c_str()) == 0;
            if (is_known) continue;
            EnsureDir(run_dir + L"\\game-new");
            const std::wstring dst = run_dir + L"\\game-new\\" + rel;
            const bool moved = MoveFileExW(p.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
            ok = ok && moved;
            r.notes.push_back(std::string(moved ? "collected new file " : "COULD NOT collect new file ") + Narrow(p));
        }
    }
    // 3. recorded files
    for (const Entry& e : entries) {
        if (e.kind != "file" || !Inside(e.path, game)) continue;
        if (e.existed) {
            if (FileExists(e.path) && Sha256File(e.path) == e.sha) continue;  // unchanged
            const bool put = !e.backup.empty() && Sha256File(e.backup) == e.sha && CopyFileW(e.backup.c_str(), e.path.c_str(), FALSE) &&
                             Sha256File(e.path) == e.sha;
            ok = ok && put;
            r.notes.push_back(std::string(put ? "restored " : "COULD NOT restore ") + Narrow(e.path));
        } else if (FileExists(e.path)) {
            SetFileAttributesW(e.path.c_str(), FILE_ATTRIBUTE_NORMAL);
            const bool del = DeleteFileW(e.path.c_str()) != FALSE;
            ok = ok && del;
            r.notes.push_back(std::string(del ? "removed (did not exist before) " : "COULD NOT remove ") + Narrow(e.path));
        }
    }
    // 4. must-not-exist paths
    for (const Entry& e : entries) {
        if (e.kind != "absent" || !Inside(e.path, game) || !FileExists(e.path)) continue;
        r.notes.push_back("UNEXPECTED file " + Narrow(e.path) + " (left in place for inspection)");
        ok = false;
    }
    // 5. stranded_together created by us and now empty
    std::string text;
    if (ReadFileBytes(run_dir + L"\\restore.txt", &text) && text.find("tree-if-empty\t") != std::string::npos) {
        const std::wstring mod = game + L"\\stranded_together";
        if (DirExists(mod) && ListFiles(mod, true).empty()) {
            RemoveTree(mod);
            r.notes.push_back("removed the empty " + Narrow(mod));
        }
    }
    r.ok = ok;
    WriteFileAtomic(run_dir + L"\\restored.txt", std::string(ok ? "ok" : "INCOMPLETE") + "\n" + IsoNow() + "\n");
    return r;
}

// ------------------------------------------------------------------ saves
namespace {
void ListDirsInto(const std::wstring& root, const std::wstring& rel, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        out.push_back(r);
        ListDirsInto(root, r, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace

std::string HashManifest(const std::wstring& dir) {
    std::string out;
    std::vector<std::wstring> dirs;
    ListDirsInto(dir, L"", dirs);
    std::sort(dirs.begin(), dirs.end());
    for (const std::wstring& rel : dirs) out += Narrow(rel) + "\\\tdir\t-\n";
    for (const std::wstring& rel : ListFiles(dir, true)) {
        const std::wstring p = dir + L"\\" + rel;
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a);
        const unsigned long long size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
        out += Narrow(rel) + "\t" + std::to_string(size) + "\t" + Sha256File(p) + "\n";
    }
    return out;
}

std::vector<std::string> DiffManifests(const std::string& before, const std::string& after) {
    const auto parse = [](const std::string& t) {
        std::map<std::string, std::string> m;
        std::istringstream in(t);
        std::string line;
        while (std::getline(in, line)) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos) m[line.substr(0, tab)] = line.substr(tab + 1);
        }
        return m;
    };
    const auto a = parse(before), b = parse(after);
    std::vector<std::string> out;
    for (const auto& [k, v] : a) {
        const auto it = b.find(k);
        if (it == b.end()) out.push_back("removed " + k);
        else if (it->second != v) out.push_back("changed " + k);
    }
    for (const auto& [k, v] : b)
        if (!a.count(k)) out.push_back("added " + k);
    return out;
}

namespace {
struct SavesRestoreFile {
    std::wstring root, backup;
};
bool ReadSavesRestore(const std::wstring& run_dir, SavesRestoreFile* m) {
    std::string text;
    if (!ReadFileBytes(run_dir + L"\\saves-restore.txt", &text)) return false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string k = line.substr(0, tab), v = line.substr(tab + 1);
        if (k == "root") m->root = Widen(v);
        else if (k == "backup") m->backup = Widen(v);
    }
    return !m->root.empty() && !m->backup.empty();
}

// The path of a DiffManifests line ("changed autosave0\checkpoint.dat", "added autosave9\").
std::wstring ChangePath(const std::string& change) {
    const auto sp = change.find(' ');
    return Widen(sp == std::string::npos ? change : change.substr(sp + 1));
}

void CopyDirs(const std::wstring& from, const std::wstring& to) {
    std::vector<std::wstring> dirs;
    ListDirsInto(from, L"", dirs);
    for (const std::wstring& d : dirs) EnsureDir(to + L"\\" + d);
}
}  // namespace

bool BackupSaves(const std::wstring& root_in, const std::wstring& run_dir, std::string* error) {
    const std::wstring root = FullPath(root_in), backup = run_dir + L"\\saves-backup";
    if (FileExists(run_dir + L"\\saves-restore.txt")) return *error = "saves-restore.txt already exists (a run folder is used once)", false;
    const std::string before = DirExists(root) ? HashManifest(root) : std::string();
    if (!WriteFileAtomic(run_dir + L"\\saves-before.txt", before)) return *error = "cannot write saves-before.txt", false;
    if (!EnsureDir(backup)) return *error = "cannot create saves-backup", false;
    if (DirExists(root)) {
        CopyDirs(root, backup);
        if (!CopyTree(root, backup)) return *error = "copying the saves to saves-backup failed", false;
    }
    if (HashManifest(backup) != before) return *error = "saves-backup does not match the saves (hash manifest differs)", false;
    const std::string m = "root\t" + Narrow(root) + "\nbackup\t" + Narrow(backup) + "\n";
    if (!WriteFileAtomic(run_dir + L"\\saves-restore.txt", m)) return *error = "cannot write saves-restore.txt", false;
    return true;
}

bool SeedScratch(const std::wstring& run_dir, const std::wstring& scratch, std::string* error) {
    SavesRestoreFile m;
    if (!ReadSavesRestore(run_dir, &m)) return *error = "no saves-restore.txt (BackupSaves first)", false;
    RemoveTree(scratch);
    if (!EnsureDir(scratch)) return *error = "cannot create the scratch folder", false;
    CopyDirs(m.backup, scratch);
    if (!CopyTree(m.backup, scratch)) return *error = "seeding the scratch copy failed", false;
    std::string before;
    ReadFileBytes(run_dir + L"\\saves-before.txt", &before);
    if (HashManifest(scratch) != before) return *error = "the scratch copy does not match the saves", false;
    // The mod's self-test asks the real root for this name; only the scratch copy has it.
    if (!WriteFileAtomic(scratch + L"\\.st-redirect-probe", "Stranded Together hidden run: the mod redirects the saves here\n"))
        return *error = "cannot write the redirect probe", false;
    return true;
}

SavesCheck CheckAndRestoreSaves(const std::wstring& run_dir) {
    SavesCheck r;
    SavesRestoreFile m;
    if (!ReadSavesRestore(run_dir, &m)) {
        r.notes.push_back("no saves-restore.txt: nothing to check");
        r.ok = true;
        return r;
    }
    r.ran = true;
    std::string before;
    ReadFileBytes(run_dir + L"\\saves-before.txt", &before);
    const std::string now = DirExists(m.root) ? HashManifest(m.root) : std::string();
    WriteFileAtomic(run_dir + L"\\saves-after.txt", now);
    r.changes = DiffManifests(before, now);
    r.unchanged = r.changes.empty();
    if (r.unchanged) {
        r.ok = true;
        WriteFileAtomic(run_dir + L"\\saves-restored.txt", "ok\nunchanged\n" + IsoNow() + "\n");
        return r;
    }
    bool ok = true;
    std::vector<std::wstring> added_dirs;
    for (const std::string& c : r.changes) {  // folders first: restored files need them
        const std::wstring rel = ChangePath(c);
        if (rel.empty() || rel.back() != L'\\') continue;
        const std::wstring real = m.root + L"\\" + rel.substr(0, rel.size() - 1);
        if (c.rfind("removed ", 0) == 0) {
            const bool made = EnsureDir(real);
            ok = ok && made;
            r.notes.push_back(std::string(made ? "recreated folder " : "COULD NOT recreate folder ") + Narrow(rel));
        } else if (c.rfind("added ", 0) == 0) {
            added_dirs.push_back(real);
        }
    }
    for (const std::string& c : r.changes) {
        const std::wstring rel = ChangePath(c);
        if (rel.empty() || rel.back() == L'\\') continue;
        const std::wstring real = m.root + L"\\" + rel, bak = m.backup + L"\\" + rel;
        if (c.rfind("added ", 0) == 0) {
            const std::wstring dst = run_dir + L"\\saves-added\\" + rel;
            EnsureDir(dst.substr(0, dst.find_last_of(L'\\')));
            SetFileAttributesW(real.c_str(), FILE_ATTRIBUTE_NORMAL);
            const bool moved = MoveFileExW(real.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != FALSE;
            ok = ok && moved;
            r.notes.push_back(std::string(moved ? "moved a new file to saves-added: " : "COULD NOT move the new file ") + Narrow(rel));
        } else {
            EnsureDir(real.substr(0, real.find_last_of(L'\\')));
            SetFileAttributesW(real.c_str(), FILE_ATTRIBUTE_NORMAL);
            const bool put = CopyFileW(bak.c_str(), real.c_str(), FALSE) != FALSE;
            ok = ok && put;
            r.notes.push_back(std::string(put ? "restored " : "COULD NOT restore ") + Narrow(rel));
        }
    }
    std::sort(added_dirs.rbegin(), added_dirs.rend());  // deepest first
    for (const std::wstring& d : added_dirs) {
        const bool gone = RemoveDirectoryW(d.c_str()) != FALSE;  // only when empty
        r.notes.push_back(std::string(gone ? "removed a new empty folder " : "kept a new folder (not empty) ") + Narrow(d));
    }
    const std::string verify = DirExists(m.root) ? HashManifest(m.root) : std::string();
    for (const std::string& l : DiffManifests(before, verify)) {
        r.notes.push_back("STILL DIFFERENT after the restore: " + l);
        ok = false;
    }
    r.ok = ok;
    WriteFileAtomic(run_dir + L"\\saves-restored.txt", std::string(r.ok ? "ok" : "INCOMPLETE") + "\nrestored\n" + IsoNow() + "\n");
    return r;
}

std::vector<std::string> CloudLogLines(const std::string& log_text, const std::string& app_id) {
    std::vector<std::string> out;
    const std::string tag = "[AppID " + app_id + "]";
    std::istringstream in(log_text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find(tag) != std::string::npos) out.push_back(line);
    }
    return out;
}

std::vector<std::string> CloudUploads(const std::vector<std::string>& app_lines) {
    std::vector<std::string> out;
    for (const std::string& l : app_lines)
        if (l.find("Need to upload file") != std::string::npos || l.find("Upload OK for file") != std::string::npos ||
            l.find("HTTP upload for file") != std::string::npos)
            out.push_back(l);
    return out;
}

// ------------------------------------------------------------------ processes
std::vector<DWORD> FindProcesses(const wchar_t* exe_name) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e)) {
        do {
            if (_wcsicmp(e.szExeFile, exe_name) == 0) out.push_back(e.th32ProcessID);
        } while (Process32NextW(snap, &e));
    }
    CloseHandle(snap);
    return out;
}

}  // namespace st::testenv
