// Stranded Together launcher core: test saves and the local two-instance test (core_sandbox.hpp).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core_sandbox.hpp"

#include <algorithm>
#include <functional>
#include <map>

#include "core_util.hpp"
#include "sandbox.hpp"   // native/mod: the low-graphics settings text shared with the mod's side

namespace st::core {
namespace {

bool AllDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
    return true;
}

bool IsSlotName(const std::wstring& name) {
    const std::wstring n = UpperInvariant(name);
    for (const wchar_t* p : {L"AUTOSAVE", L"MANUALSAVE", L"QUICKSAVE"}) {
        const std::wstring prefix = p;
        if (n.size() > prefix.size() && n.compare(0, prefix.size(), prefix) == 0 && AllDigits(n.substr(prefix.size()))) return true;
    }
    return false;
}

int CountSlots(const std::wstring& dir) {
    if (!IsDir(dir)) return 0;
    int n = 0;
    for (const DirEntry& e : ListDirectory(dir))
        if (e.directory && !e.reparse && IsSlotName(e.name) && IsFile(JoinPath2(JoinPath2(dir, e.name), L"checkpoint.dat"))) ++n;
    return n;
}

std::vector<std::wstring> DigitDirs(const std::wstring& dir) {
    std::vector<std::wstring> out;
    if (!IsDir(dir)) return out;
    for (const DirEntry& e : ListDirectory(dir))
        if (e.directory && !e.reparse && AllDigits(e.name)) out.push_back(e.name);
    return out;
}

bool SandboxPlaceOk(const InstancePaths& paths, const SandboxPaths& sandbox, const std::wstring& real_root, std::string& error) {
    const std::wstring base = FullPath(JoinPath2(paths.mod, L"sandbox"));
    const std::wstring root = FullPath(sandbox.root);
    if (!PathInside(root, base) || IEquals(root, base)) {
        error = "The test-save folder must be inside stranded_together\\sandbox.";
        return false;
    }
    if (!real_root.empty()) {
        const std::wstring real = FullPath(real_root);
        if (PathInside(root, real) || PathInside(real, root)) {
            error = "The test-save folder and your save folder overlap: nothing was copied.";
            return false;
        }
    }
    return true;
}

void ManifestOf(const std::wstring& root, const std::wstring& rel, std::vector<std::string>& lines) {
    const std::wstring dir = rel.empty() ? root : JoinPath2(root, rel);
    for (const DirEntry& e : ListDirectory(dir)) {
        if (e.reparse) continue;
        const std::wstring child = rel.empty() ? e.name : rel + L"\\" + e.name;
        if (e.directory) {
            lines.push_back(Utf8(child) + "\\\t-\t-");
            ManifestOf(root, child, lines);
        } else {
            std::string hex;
            std::uint64_t size = 0;
            if (!Sha256FileHex(JoinPath2(root, child), hex, &size)) hex = "unreadable";
            lines.push_back(Utf8(child) + "\t" + std::to_string(size) + "\t" + hex);
        }
    }
}

}  // namespace

SandboxPaths GetSandboxPaths(const InstancePaths& paths, const std::string& name) {
    SandboxPaths s;
    s.root = JoinPath2(JoinPath2(paths.mod, L"sandbox"), Wide(name));
    s.saves = JoinPath2(s.root, L"saves");
    s.settings = JoinPath2(s.root, L"settings.cfg");
    s.marker = JoinPath2(s.root, L"sandbox.json");
    return s;
}

bool SandboxSeeded(const SandboxPaths& sandbox) { return IsFile(JoinPath2(sandbox.saves, kProbeName)); }

SeedCheck CheckSeedFolder(const std::wstring& folder_in) {
    SeedCheck c;
    if (folder_in.empty()) {
        c.note = "Pick a folder with a DEATH STRANDING save in it.";
        return c;
    }
    const std::wstring folder = FullPath(folder_in);
    if (!IsDir(folder)) {
        c.note = "That folder does not exist any more.";
        return c;
    }
    const std::wstring real = RealSaveRoot();
    if (!real.empty() && PathInside(folder, FullPath(real))) {
        // Allowed (it is only read), but say what it is.
        c.note = "your own save folder: ";
    }
    std::vector<std::wstring> tries;
    for (const std::wstring& base : {JoinPath2(folder, L"DeathStrandingDC"), folder}) {
        tries.push_back(base);
        for (const std::wstring& d : DigitDirs(base)) tries.push_back(JoinPath2(base, d));
    }
    for (const std::wstring& t : tries) {
        const int n = CountSlots(t);
        if (n > 0) {
            c.ok = true;
            c.slots_dir = t;
            c.slots = n;
            c.note += std::to_string(n) + (n == 1 ? " save slot" : " save slots") + " found";
            return c;
        }
    }
    c.note = "No save slots there. Pick the folder that holds DeathStrandingDC (with autosave/manualsave folders inside).";
    return c;
}

std::wstring RealSaveRoot() {
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, 1024);
    if (n == 0 || n >= 1024) return {};
    return JoinPath2(std::wstring(buf, n), L"KojimaProductions");
}

bool SeedSandbox(const InstancePaths& paths, const SandboxPaths& sandbox, const SeedCheck* seed, const std::wstring& real_root,
                 std::uint32_t account, const std::string& seed_label, std::string& error) {
    if (!SandboxPlaceOk(paths, sandbox, real_root, error)) return false;
    if (seed && !seed->ok) {
        error = seed->note.empty() ? "The test save folder has no save slots." : seed->note;
        return false;
    }
    if (seed && PathInside(FullPath(seed->slots_dir), FullPath(JoinPath2(paths.mod, L"sandbox")))) {
        // It would be emptied below before it is copied (or be another test copy).
        error = "Pick a test save outside stranded_together\\sandbox (that folder holds the test copies themselves).";
        return false;
    }
    // The probe first: a sandbox that could not be emptied completely is never taken for seeded.
    DeleteFileChecked(JoinPath2(sandbox.saves, kProbeName));
    RemoveTree(sandbox.saves);
    if (!CreateDirectories(sandbox.saves)) {
        error = "Could not create " + Utf8(sandbox.saves) + ".";
        return false;
    }
    const std::wstring real_dc = real_root.empty() ? std::wstring() : JoinPath2(real_root, L"DeathStrandingDC");
    std::vector<std::wstring> accounts;
    if (account) accounts.push_back(std::to_wstring(account));
    else accounts = DigitDirs(real_dc);
    if (accounts.empty()) {
        error = "Your Steam account is not known (start Steam and log in), so the test save has no account folder.";
        return false;
    }
    for (const std::wstring& a : accounts) {
        const std::wstring src = real_dc.empty() ? std::wstring() : JoinPath2(real_dc, a);
        const std::wstring dst = JoinPath2(JoinPath2(sandbox.saves, L"DeathStrandingDC"), a);
        if (!CreateDirectories(dst)) {
            error = "Could not create the test-save account folder.";
            return false;
        }
        // Your profile (options, key bindings, the account-bound profile.dat): never another account's,
        // which makes the game stall before its first frame (docs/hidden-testing.md "Using another save").
        if (!src.empty() && IsDir(src)) {
            for (const DirEntry& e : ListDirectory(src)) {
                if (e.reparse) continue;
                const bool slot = e.directory && IsSlotName(e.name);
                if (slot && seed) continue;               // the test save's slots instead
                if (!e.directory && IEquals(e.name, L"steam_autocloud.vdf")) continue;   // Steam's own file
                const std::wstring from = JoinPath2(src, e.name), to = JoinPath2(dst, e.name);
                const bool ok = e.directory ? CopyTree(from, to, error) : CopyFileChecked(from, to, true, error);
                if (!ok) return false;
            }
        }
        if (seed) {
            for (const DirEntry& e : ListDirectory(seed->slots_dir)) {
                if (!e.directory || e.reparse || !IsSlotName(e.name)) continue;
                if (!CopyTree(JoinPath2(seed->slots_dir, e.name), JoinPath2(dst, e.name), error)) return false;
            }
        }
        CreateDirectories(JoinPath2(JoinPath2(JoinPath2(sandbox.saves, L"DeathStranding"), a), L"transfer"));
    }
    std::string werr;
    if (!WriteAtomic(JoinPath2(sandbox.saves, kProbeName), std::string("Stranded Together test save: the mod redirects the saves here\n"), true,
                     werr)) {
        error = "Could not write the test-save probe file: " + werr;
        return false;
    }
    std::string marker = "{\"v\":1,\"seed\":\"";
    for (char ch : seed_label) marker += (ch == '"' || ch == '\\') ? '_' : ch;
    marker += "\",\"slots\":" + std::to_string(seed ? seed->slots : 0) + "}\n";
    WriteAtomic(sandbox.marker, marker, true, werr);
    return true;
}

bool ResetSandbox(const InstancePaths& paths, const SandboxPaths& sandbox, std::string& error) {
    if (!SandboxPlaceOk(paths, sandbox, RealSaveRoot(), error)) return false;
    if (!IsDir(sandbox.root)) return true;
    if (!RemoveTree(sandbox.root)) {
        error = "Some test-save files could not be deleted (is the game still running?).";
        return false;
    }
    return true;
}

std::string SaveTreeManifest(const std::wstring& root) {
    if (root.empty() || !IsDir(root)) return {};
    std::vector<std::string> lines;
    ManifestOf(root, L"", lines);
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const std::string& l : lines) out += l + "\n";
    return out;
}

std::vector<std::string> ManifestChanges(const std::string& before, const std::string& after) {
    const auto parse = [](const std::string& text) {
        std::map<std::string, std::string> m;
        for (const std::string& l : SplitLines(text)) {
            const auto tab = l.find('\t');
            if (tab == std::string::npos) continue;
            m[l.substr(0, tab)] = l.substr(tab + 1);
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

std::string LocalTestSettingsCfg() { return st::sandbox::LowGraphicsSettingsCfg(); }

}  // namespace st::core
