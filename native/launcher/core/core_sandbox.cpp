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

bool IsSaveSlotName(const std::wstring& name) { return IsSlotName(name); }

// ---- The host-held save session ----------------------------------------------------------------
namespace {

// The first link (junction, symbolic link, mount point) anywhere below `dir`, relative to it; "" when there is none.
// Links are never followed. Deeper than `depth` levels counts as a link too (the co-op folder is three levels deep).
std::wstring FirstLinkBelow(const std::wstring& dir, const std::wstring& rel, int depth) {
    for (const DirEntry& e : ListDirectory(rel.empty() ? dir : JoinPath2(dir, rel))) {
        const std::wstring child = rel.empty() ? e.name : rel + L"\\" + e.name;
        if (e.reparse) return child;
        if (!e.directory) continue;
        if (depth <= 0) return child + L" (too deep)";
        const std::wstring below = FirstLinkBelow(dir, child, depth - 1);
        if (!below.empty()) return below;
    }
    return {};
}

}  // namespace

bool SeedSessionGuest(const InstancePaths& paths, const std::wstring& real_root, std::uint32_t account, SessionGuestReport& report,
                      std::string& error) {
    report = SessionGuestReport{};
    const SandboxPaths sb = GetSandboxPaths(paths, kCoopGuestSandbox);
    if (!SandboxPlaceOk(paths, sb, real_root, error)) return false;
    if (!account) {
        error = "Your Steam account is not known (start Steam and log in), so the co-op folder has no account folder.";
        return false;
    }
    if (HasReparsePoint(sb.saves, paths.game)) {
        error = "The co-op save folder (stranded_together\\sandbox\\coop-guest) is a link (junction or symbolic link). Make it a normal folder.";
        return false;
    }
    // Nothing below it may be a link either: the wipe below deletes slot folders and other accounts' folders, and a link
    // there (say DeathStrandingDC\<account> pointing at the real save folder) would take the real saves with it. Checked
    // before anything is deleted; links are never followed.
    if (IsDir(sb.saves)) {
        const std::wstring link = FirstLinkBelow(sb.saves, L"", 12);
        if (!link.empty()) {
            error = "A folder inside the co-op save folder (stranded_together\\sandbox\\coop-guest\\saves\\" + Utf8(link) +
                    ") is a link (junction or symbolic link). Nothing was deleted. Remove the link (or make it a normal folder).";
            return false;
        }
    }
    // The probe first: a folder that could not be prepared completely is never taken for seeded.
    if (IsFile(JoinPath2(sb.saves, kProbeName)) && !DeleteFileChecked(JoinPath2(sb.saves, kProbeName))) {
        error = "The co-op save folder is in use (is the game still running?).";
        return false;
    }
    if (!CreateDirectories(sb.saves)) {
        error = "Could not create " + Utf8(sb.saves) + ".";
        return false;
    }
    const std::wstring incoming = JoinPath2(sb.saves, L"incoming");
    if (IsDir(incoming)) {
        if (!RemoveTree(incoming)) {
            error = "The last session's received save could not be deleted (is the game still running?).";
            return false;
        }
        report.wiped_incoming = true;
    }
    const std::wstring dc = JoinPath2(sb.saves, L"DeathStrandingDC");
    const std::wstring me = std::to_wstring(account);
    for (const DirEntry& e : ListDirectory(dc)) {
        if (!e.directory || e.reparse || !AllDigits(e.name) || e.name == me) continue;
        if (!RemoveTree(JoinPath2(dc, e.name))) {
            error = "An old account folder in the co-op save folder could not be deleted (is the game still running?).";
            return false;
        }
        ++report.removed_accounts;
    }
    const std::wstring dst = JoinPath2(dc, me);
    if (!CreateDirectories(dst)) {
        error = "Could not create the co-op save folder's account folder.";
        return false;
    }
    if (HasReparsePoint(dst, sb.saves)) {   // made a link since the check above: still never wiped through it
        error = "The co-op save folder's account folder is a link (junction or symbolic link). Nothing was deleted.";
        return false;
    }
    // The last session's copies of the host's save (and the guest game's own autosaves of that world).
    for (const DirEntry& e : ListDirectory(dst)) {
        if (!e.directory || !IsSlotName(e.name)) continue;
        if (e.reparse || !RemoveTree(JoinPath2(dst, e.name))) {
            error = "The last co-op session's saves could not be deleted (is the game still running?).";
            return false;
        }
        ++report.wiped_slots;
    }
    // Your own profile and settings, only what is missing (the co-op folder keeps its own after the first session).
    const std::wstring src = real_root.empty() ? std::wstring() : JoinPath2(JoinPath2(real_root, L"DeathStrandingDC"), me);
    if (!src.empty() && IsDir(src)) {
        for (const DirEntry& e : ListDirectory(src)) {
            if (e.reparse) continue;
            const std::wstring from = JoinPath2(src, e.name), to = JoinPath2(dst, e.name);
            if (e.directory) {
                if (!IEquals(e.name, L"profile") || IsDir(to)) continue;   // slots and anything else: never
                if (!CopyTree(from, to, error)) return false;
            } else {
                if (IEquals(e.name, L"steam_autocloud.vdf") || IsFile(to)) continue;   // Steam's own file
                if (!CopyFileChecked(from, to, false, error)) return false;
            }
            ++report.copied;
        }
    }
    CreateDirectories(JoinPath2(JoinPath2(JoinPath2(sb.saves, L"DeathStranding"), me), L"transfer"));
    std::string werr;
    if (!WriteAtomic(JoinPath2(sb.saves, kProbeName), std::string("Stranded Together co-op guest folder: the mod redirects the saves here\n"),
                     true, werr)) {
        error = "Could not write the co-op folder's probe file: " + werr;
        return false;
    }
    WriteAtomic(sb.marker, std::string("{\"v\":1,\"seed\":\"session-guest\",\"slots\":0}\n"), true, werr);
    return true;
}

std::wstring HostBackupsDir(const InstancePaths& paths) { return JoinPath2(paths.mod, L"backups"); }

namespace {

// yyyyMMdd-HHmmss with an optional -N (two backups in one second).
bool IsBackupStampName(const std::wstring& name) {
    if (name.size() < 15 || name.size() > 18 || name[8] != L'-') return false;
    for (std::size_t i = 0; i < 15; ++i)
        if (i != 8 && (name[i] < L'0' || name[i] > L'9')) return false;
    if (name.size() == 15) return true;
    return name[15] == L'-' && AllDigits(name.substr(16));
}

// Ours (backup.json with our product), and its sequence number ("seq": 0 for a backup.json without one).
bool IsOurBackup(const std::wstring& dir, std::uint64_t* seq = nullptr) {
    std::string text;
    if (!ReadTextShared(JoinPath2(dir, L"backup.json"), text, 1u << 16) ||
        text.find("\"product\":\"stranded-together-backup\"") == std::string::npos)
        return false;
    if (seq) {
        *seq = 0;
        const std::size_t at = text.find("\"seq\":");
        for (std::size_t i = at == std::string::npos ? text.size() : at + 6; i < text.size() && text[i] >= '0' && text[i] <= '9' && *seq < (1ull << 50); ++i)
            *seq = *seq * 10 + static_cast<std::uint64_t>(text[i] - '0');
    }
    return true;
}

struct OurBackup {
    std::wstring name;
    std::uint64_t seq = 0;
};
std::vector<OurBackup> ListOurBackups(const std::wstring& base) {
    std::vector<OurBackup> out;
    for (const DirEntry& e : ListDirectory(base)) {
        if (!e.directory || e.reparse || !IsBackupStampName(e.name)) continue;
        OurBackup b;
        b.name = e.name;
        if (IsOurBackup(JoinPath2(base, e.name), &b.seq)) out.push_back(b);
    }
    return out;
}

}  // namespace

bool BackupHostSaves(const InstancePaths& paths, const std::wstring& real_root, std::uint32_t account, const std::string& stamp, int keep,
                     HostBackupReport& report, std::string& error) {
    report = HostBackupReport{};
    const std::wstring base = HostBackupsDir(paths);
    const std::wstring stamp_w = Wide(stamp);
    if (stamp_w.size() != 15 || !IsBackupStampName(stamp_w)) {
        error = "The backup name must be a time stamp (yyyyMMdd-HHmmss).";
        return false;
    }
    if (!account) {
        error = "Your Steam account is not known (start Steam and log in), so your saves could not be backed up.";
        return false;
    }
    if (real_root.empty()) {
        error = "Your save folder is not known (LOCALAPPDATA is not set), so your saves could not be backed up.";
        return false;
    }
    const std::wstring real = FullPath(real_root), full_base = FullPath(base);
    if (PathInside(full_base, real) || PathInside(real, full_base)) {
        error = "The backup folder and your save folder overlap: nothing was copied.";
        return false;
    }
    const std::wstring me = std::to_wstring(account);
    const std::wstring src = JoinPath2(JoinPath2(real_root, L"DeathStrandingDC"), me);
    if (!IsDir(src)) return true;   // no saves yet (a first game): nothing to keep
    if (!CreateDirectories(base) || HasReparsePoint(base, paths.game)) {
        error = "Could not create the backup folder (stranded_together\\backups), or it is a link.";
        return false;
    }
    std::wstring name = stamp_w;
    for (int n = 2; PathExists(JoinPath2(base, name)) && n <= 99; ++n) name = stamp_w + L"-" + std::to_wstring(n);
    const std::wstring final_dir = JoinPath2(base, name);
    const std::wstring partial = final_dir + L".partial";
    if (PathExists(final_dir) || (PathExists(partial) && !RemoveTree(partial))) {
        error = "The backup folder for this time stamp is in use.";
        return false;
    }
    const std::wstring dst = JoinPath2(JoinPath2(partial, L"DeathStrandingDC"), me);
    if (!CreateDirectories(dst)) {
        error = "Could not create the backup folder.";
        return false;
    }
    const auto fail = [&](const std::string& why) {
        RemoveTree(partial);
        error = "Your saves could not be backed up: " + why;
        return false;
    };
    for (const DirEntry& e : ListDirectory(src)) {
        if (e.reparse) continue;
        const std::wstring from = JoinPath2(src, e.name), to = JoinPath2(dst, e.name);
        std::string why;
        if (e.directory) {
            if (!CopyTree(from, to, why)) return fail(why);
            if (IsSlotName(e.name)) ++report.slots;
            for (const std::string& line : SplitLines(SaveTreeManifest(to)))
                if (!line.empty() && line.find("\\\t-\t-") == std::string::npos) ++report.files;
        } else {
            if (IEquals(e.name, L"steam_autocloud.vdf")) continue;
            if (!CopyFileChecked(from, to, false, why)) return fail(why);
            ++report.files;
        }
    }
    // "seq" orders the backups (the local-time stamp in the name does not: a clock set ahead once, or a DST fall-back,
    // gives older backups later names): one more than the highest seq of our complete backups.
    std::uint64_t seq = 1;
    for (const OurBackup& b : ListOurBackups(base))
        if (b.seq >= seq) seq = b.seq + 1;
    std::string werr;
    const std::string marker = "{\"v\":1,\"product\":\"stranded-together-backup\",\"stamp\":\"" + Utf8(name) + "\",\"seq\":" +
                               std::to_string(seq) + ",\"slots\":" + std::to_string(report.slots) + ",\"files\":" + std::to_string(report.files) +
                               "}\n";
    if (!WriteAtomic(JoinPath2(partial, L"backup.json"), marker, true, werr)) return fail(werr);
    if (!MovePath(partial, final_dir, werr)) return fail(werr);
    report.dir = final_dir;
    // The newest `keep` complete backups stay (by seq, then name); never the one just made. Half-done ones of earlier
    // launches go too. Only our own folders.
    for (const DirEntry& e : ListDirectory(base)) {
        if (!e.directory || e.reparse) continue;
        if (e.name.size() > 8 && e.name.compare(e.name.size() - 8, 8, L".partial") == 0 && IsBackupStampName(e.name.substr(0, e.name.size() - 8)))
            RemoveTree(JoinPath2(base, e.name));
    }
    std::vector<OurBackup> complete = ListOurBackups(base);
    std::sort(complete.begin(), complete.end(), [](const OurBackup& a, const OurBackup& b) {
        return a.seq != b.seq ? a.seq < b.seq : a.name < b.name;
    });
    const std::size_t kept = keep < 1 ? 1u : static_cast<std::size_t>(keep);
    const std::size_t prune = complete.size() > kept ? complete.size() - kept : 0;
    for (std::size_t i = 0; i < complete.size(); ++i) {
        if (IEquals(complete[i].name, name)) continue;   // the backup of this session is never pruned
        if (i < prune) {
            if (RemoveTree(JoinPath2(base, complete[i].name))) ++report.pruned;
        } else if (complete[i].name.compare(0, 15, name, 0, 15) > 0) {
            ++report.newer_named;   // kept, with a later time stamp than this backup's
        }
    }
    return true;
}

std::string NonSlotManifest(const std::string& manifest) {
    std::string out;
    for (const std::string& line : SplitLines(manifest)) {
        if (line.empty()) continue;
        const std::size_t tab = line.find('\t');
        const std::string path = tab == std::string::npos ? line : line.substr(0, tab);
        bool slot = false;
        std::size_t at = 0;
        while (!slot && at < path.size()) {
            std::size_t sep = path.find('\\', at);
            if (sep == std::string::npos) sep = path.size();
            if (sep > at) slot = IsSlotName(Wide(path.substr(at, sep - at)));
            at = sep + 1;
        }
        if (!slot) out += line + "\n";
    }
    return out;
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

std::string LocalTestSettingsCfg(const std::string& current) {
    return st::sandbox::MergeGameSettings(current, st::sandbox::LowGraphicsSettingsCfg());
}

}  // namespace st::core
