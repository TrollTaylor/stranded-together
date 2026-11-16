// Stranded Together launcher: the updater's tests (no network, no window).
// SemVer, the GitHub release feed (drafts, prereleases, source zips, foreign
// URLs, digests), the release zip's verification, the install with its
// rollback paths (a failed move, a failed restart, ds.exe running, a foreign
// winhttp.dll, a mod switched off), leftovers, the real model's Check for
// updates / Update now on a fake feed and a fake game folder, the update
// banner logic, the command line, the mock's simulation, and the real WinHTTP
// client against an HTTP server on 127.0.0.1.
//
//   st_launcher_update_tests                       the tests
//   st_launcher_update_tests --check-imports <exe> StrandedTogether.exe imports no winhttp.dll (static
//                                                  or delay-loaded) and keeps /DEPENDENTLOADFLAG:0x800
//   st_launcher_update_tests --verify-zip <zip> <version>  the launcher's check of a release zip (tools/release.ps1)
//   st_launcher_update_tests --live-feed [version] reads the real GitHub feed once (manual; not in ctest)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/core_install.hpp"
#include "core/core_model.hpp"
#include "core/core_paths.hpp"
#include "core/core_update.hpp"
#include "core/core_util.hpp"
#include "launcher_model_mock.hpp"
#include "launcher_ui_logic.hpp"
#include "zip.hpp"

using namespace st;
using namespace st::core;

namespace {
int g_checks = 0, g_failures = 0;

void Check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL (line %d): %s\n", line, what);
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

// ---- Fixtures ---------------------------------------------------------------------------
class FakeEnv final : public Env {
public:
    std::int64_t now = 1763208000000;
    std::int64_t NowMs() override { return now; }
    void SleepMs(std::uint32_t ms) override { now += ms; }
    Civil LocalTime(std::int64_t utc_ms) override { return CivilFromMs(utc_ms); }
    bool RegDword(bool, const std::wstring&, const std::wstring&, std::uint32_t&) override { return false; }
    bool RegString(bool, const std::wstring&, const std::wstring&, std::wstring&) override { return false; }
    std::vector<ProcessRow> GameProcesses() override { return game_running ? std::vector<ProcessRow>{game} : std::vector<ProcessRow>{}; }
    bool ProcessStartMs(std::uint32_t, std::int64_t&) override { return false; }
    bool ProcessAlive(std::uint32_t pid) override { return pid == 9 && game_running; }
    std::uint32_t CurrentPid() override { return 4242; }
    std::int64_t CurrentStartMs() override { return 0; }
    bool OpenUrl(const std::wstring&) override { return true; }
    bool StartWatchdog(const std::wstring&, const std::string&, std::uint32_t& pid) override {
        pid = 0;
        return false;
    }
    bool SetClipboardText(const std::string&) override { return true; }
    bool GetClipboardText(std::string&) override { return false; }
    bool ClearClipboard() override { return true; }
    std::wstring DesktopDir() override { return {}; }
    std::wstring TempDir() override { return {}; }
    std::string OsVersion() override { return "test"; }

    bool game_running = false;
    ProcessRow game;
};

// A fake HTTP seam: URL -> body (Get) or file bytes (Download).
class FakeHttp final : public HttpClient {
public:
    std::map<std::string, std::string> pages, files;
    std::vector<std::string> requested;
    bool offline = false;
    bool Get(const std::string& url, std::uint64_t max_bytes, std::string& body, std::string& error) override {
        requested.push_back(url);
        auto it = pages.find(url);
        if (offline || it == pages.end()) {
            error = offline ? "offline" : "HTTP 404";
            return false;
        }
        if (it->second.size() > max_bytes) {
            error = "too large";
            return false;
        }
        body = it->second;
        return true;
    }
    bool Download(const std::string& url, const std::wstring& dest, std::uint64_t max_bytes, const std::function<void(const HttpProgress&)>& progress,
                  const std::atomic<bool>*, std::string& error) override {
        requested.push_back(url);
        auto it = files.find(url);
        if (offline || it == files.end() || it->second.size() > max_bytes) {
            error = "download failed";
            return false;
        }
        if (progress) progress(HttpProgress{it->second.size() / 2, it->second.size()});
        if (!WriteAtomic(dest, it->second, true, error)) return false;
        if (progress) progress(HttpProgress{it->second.size(), it->second.size()});
        return true;
    }
};

std::wstring NewTempDir(const wchar_t* prefix) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + prefix + Wide(RandomHex(6));
    CreateDirectories(dir);
    return dir;
}

std::string FakeOurDll(const std::string& salt = "") {
    std::string b = "MZ";
    b.append(62, '\0');
    b += "DllCanUnloadNow=\\\\.\\GLOBALROOT\\SystemRoot\\System32\\winhttp.dll.DllCanUnloadNow";
    b.append(8, '\0');
    for (char c : std::string("stranded_together")) {
        b.push_back(c);
        b.push_back('\0');
    }
    b += salt;
    return b;
}

std::string FakeExe(const std::string& salt) {
    std::string b = "MZ";
    b.append(62, '\0');
    return b + "StrandedTogether launcher " + salt;
}

struct ZipFile {
    std::string name, data;
    bool listed = true;   // in package.json files[]
    std::string listed_sha;   // "" = the real hash
};

// A release zip as tools/package.ps1 writes it, with the given package.json version.
std::string MakeReleaseZip(const std::string& version, const std::vector<ZipFile>& files, bool with_package = true,
                           const std::string& product = "stranded-together-package") {
    ZipWriter w;
    std::string error;
    std::string rows;
    for (const ZipFile& f : files) {
        w.AddFile(f.name, f.data.data(), f.data.size(), error);
        if (!f.listed) continue;
        if (!rows.empty()) rows += ",";
        rows += "{\"path\":\"" + f.name + "\",\"sha256\":\"" + (f.listed_sha.empty() ? Sha256Hex(f.data) : f.listed_sha) +
                "\",\"size\":" + std::to_string(f.data.size()) + "}";
    }
    if (with_package) {
        const std::string pkg = "{\"product\":\"" + product + "\",\"v\":1,\"version\":\"" + version + "\",\"build_id\":\"" + version +
                                "+abc1234\",\"files\":[" + rows + "]}";
        w.AddFile("stranded_together/package.json", pkg.data(), pkg.size(), error);
    }
    std::vector<std::uint8_t> out;
    w.Finish(out, error);
    return std::string(out.begin(), out.end());
}

std::vector<ZipFile> StandardFiles(const std::string& salt) {
    return {{"StrandedTogether.exe", FakeExe(salt)},
            {"winhttp.dll", FakeOurDll(salt)},
            {"stranded_together/README.txt", "readme " + salt},
            {"stranded_together/THIRD_PARTY_NOTICES.txt", "notices " + salt}};
}

std::string ReadAll(const std::wstring& path) {
    std::string text;
    ReadTextShared(path, text, 64u << 20);
    return text;
}

bool PutFile(const std::wstring& path, const std::string& data) {
    std::string error;
    CreateDirectories(ParentOf(path));
    return WriteAtomic(path, data, true, error);
}

// A fake game folder holding an installed release `salt`.
InstancePaths MakeGameFolder(const std::wstring& dir, const std::string& salt) {
    const InstancePaths paths = GetInstancePaths(dir);
    PutFile(paths.exe, "ds");
    PutFile(paths.launcher_exe, FakeExe(salt));
    PutFile(paths.mod_dll, FakeOurDll(salt));
    PutFile(JoinPath2(paths.mod, L"README.txt"), "readme " + salt);
    PutFile(JoinPath2(paths.mod, L"THIRD_PARTY_NOTICES.txt"), "notices " + salt);
    PutFile(paths.package_json, "{\"product\":\"stranded-together-package\",\"v\":1,\"version\":\"0.1.0\",\"files\":[]}");
    PutFile(paths.settings, "{\"transport\":\"steam\"}");
    return paths;
}

bool NoStagingLeft(const InstancePaths& paths) {
    for (const std::wstring& dir : {paths.game, paths.mod})
        for (const DirEntry& e : ListDirectory(dir)) {
            const std::string n = LowerAscii(Utf8(e.name));
            if (EndsWith(n, ".st-new") || EndsWith(n, ".st-old")) return false;
        }
    return true;
}

// ---- SemVer --------------------------------------------------------------------------------
void SemVerTests() {
    SemVer v;
    CHECK(ParseSemVer("1.2.3", v) && v.major == 1 && v.minor == 2 && v.patch == 3 && v.pre.empty());
    CHECK(ParseSemVer("v0.14.2", v) && v.minor == 14 && v.patch == 2);
    CHECK(ParseSemVer("0.1.0+abc1234", v) && FormatSemVer(v) == "0.1.0");
    CHECK(ParseSemVer("1.0.0-rc.1+build.5", v) && v.pre.size() == 2 && v.pre[0] == "rc" && v.pre[1] == "1" && FormatSemVer(v) == "1.0.0-rc.1");
    CHECK(!ParseSemVer("", v));
    CHECK(!ParseSemVer("1.2", v));
    CHECK(!ParseSemVer("1.2.3.4", v));
    CHECK(!ParseSemVer("1.2.x", v));
    CHECK(!ParseSemVer("1.2.3-", v));
    CHECK(!ParseSemVer("1.2.3-rc..1", v));
    CHECK(!ParseSemVer("1.2.3+", v));
    CHECK(!ParseSemVer("99999999999.0.0", v));
    CHECK(!ParseSemVer("vv1.2.3", v));
    CHECK(!ParseSemVer("source-1.2.3", v));

    CHECK(IsVersionNewer("0.2.0", "0.1.0"));
    CHECK(IsVersionNewer("v0.1.1", "0.1.0+abc1234"));
    CHECK(IsVersionNewer("0.10.0", "0.9.9"));
    CHECK(IsVersionNewer("1.0.0", "0.99.99"));
    CHECK(!IsVersionNewer("0.1.0", "0.1.0+abc1234"));   // build metadata is not newer
    CHECK(!IsVersionNewer("0.1.0", "0.2.0"));
    CHECK(!IsVersionNewer("garbage", "0.1.0"));
    CHECK(!IsVersionNewer("0.2.0", "garbage"));
    CHECK(IsVersionNewer("1.0.0", "1.0.0-rc.1"));
    CHECK(!IsVersionNewer("1.0.0-rc.1", "1.0.0"));
    // SemVer 2.0 example order: 1.0.0-alpha < -alpha.1 < -alpha.beta < -beta < -beta.2 < -beta.11 < -rc.1 < 1.0.0
    const char* order[] = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"};
    for (std::size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); ++i) {
        CHECK(IsVersionNewer(order[i + 1], order[i]));
        CHECK(!IsVersionNewer(order[i], order[i + 1]));
    }
    SemVer a, b;
    CHECK(ParseSemVer("1.0.0-007", a) && ParseSemVer("1.0.0-7", b) && CompareSemVer(a, b) == 0);
    CHECK(UpdateAssetName("0.2.0") == "StrandedTogether-0.2.0.zip");
}

// ---- The release feed --------------------------------------------------------------------------
std::string Asset(const std::string& name, const std::string& url, std::uint64_t size = 1234, const std::string& digest = "") {
    return "{\"name\":\"" + name + "\",\"state\":\"uploaded\",\"size\":" + std::to_string(size) + ",\"browser_download_url\":\"" + url + "\"" +
           (digest.empty() ? std::string() : ",\"digest\":\"" + digest + "\"") + "}";
}
std::string GitHubUrl(const std::string& tag, const std::string& name) { return std::string(kReleaseDownloadPrefix) + tag + "/" + name; }
std::string Release(const std::string& tag, const std::string& assets, bool draft = false, bool prerelease = false) {
    return "{\"tag_name\":\"" + tag + "\",\"name\":\"Stranded Together " + tag + "\",\"body\":\"## Notes\\n- one\",\"draft\":" +
           (draft ? "true" : "false") + ",\"prerelease\":" + (prerelease ? "true" : "false") +
           ",\"html_url\":\"https://github.com/TrollTaylor/stranded-together/releases/tag/" + tag +
           "\",\"zipball_url\":\"https://api.github.com/repos/TrollTaylor/stranded-together/zipball/" + tag + "\",\"assets\":[" + assets + "]}";
}

void FeedTests() {
    UpdateInfo u;
    std::string error;
    const std::string digest = "sha256:" + std::string(64, 'a');
    const std::string feed = "[" +
        Release("v0.5.0", Asset("StrandedTogether-0.5.0.zip", GitHubUrl("v0.5.0", "StrandedTogether-0.5.0.zip")), true) + "," +   // draft
        Release("v0.4.0", Asset("StrandedTogether-0.4.0.zip", GitHubUrl("v0.4.0", "StrandedTogether-0.4.0.zip")), false, true) + "," +   // prerelease
        Release("v0.3.0", Asset("StrandedTogether-source-0.3.0.zip", GitHubUrl("v0.3.0", "StrandedTogether-source-0.3.0.zip"))) + "," +   // source only
        Release("v0.2.5", Asset("StrandedTogether-0.2.5.zip", "https://evil.example/StrandedTogether-0.2.5.zip")) + "," +   // foreign URL
        Release("v0.2.4", Asset("StrandedTogether-0.2.4.zip", GitHubUrl("v0.2.4", "StrandedTogether-0.2.4.zip")) + "," +
                              Asset("StrandedTogether-0.2.4.zip", GitHubUrl("v0.2.4", "StrandedTogether-0.2.4.zip"))) + "," +   // ambiguous
        Release("v0.2.3-beta.1", Asset("StrandedTogether-0.2.3-beta.1.zip", GitHubUrl("v0.2.3-beta.1", "StrandedTogether-0.2.3-beta.1.zip"))) + "," +
        Release("nightly", Asset("StrandedTogether-nightly.zip", GitHubUrl("nightly", "StrandedTogether-nightly.zip"))) + "," +
        Release("v0.2.0", Asset("StrandedTogether-source-0.2.0.zip", GitHubUrl("v0.2.0", "StrandedTogether-source-0.2.0.zip")) + "," +
                              Asset("StrandedTogether-0.2.0.zip", GitHubUrl("v0.2.0", "StrandedTogether-0.2.0.zip"), 5555, digest)) + "," +
        Release("v0.1.5", Asset("StrandedTogether-0.1.5.zip", GitHubUrl("v0.1.5", "StrandedTogether-0.1.5.zip"))) + "]";
    CHECK(ParseReleasesFeed(feed, "0.1.0+abc1234", u, error));
    CHECK(u.status == UpdateStatus::available && u.latest_version == "0.2.0" && u.tag == "v0.2.0");
    CHECK(u.asset_name == "StrandedTogether-0.2.0.zip" && u.download_url == GitHubUrl("v0.2.0", "StrandedTogether-0.2.0.zip"));
    CHECK(u.download_size == 5555 && u.asset_sha256 == std::string(64, 'A'));
    CHECK(u.release_notes == "## Notes\n- one" && u.html_url.find("/releases/tag/v0.2.0") != std::string::npos);
    CHECK(u.current_version == "0.1.0+abc1234");

    // Prereleases when asked for: v0.4.0 (published as prerelease) wins; the draft never.
    FeedOptions pre;
    pre.allow_prerelease = true;
    CHECK(ParseReleasesFeed(feed, "0.1.0", u, error, pre) && u.latest_version == "0.4.0");

    // Same version: up to date; an older one too.
    CHECK(ParseReleasesFeed(feed, "0.2.0", u, error) && u.status == UpdateStatus::up_to_date && u.latest_version == "0.2.0");
    CHECK(ParseReleasesFeed(feed, "0.3.0", u, error) && u.status == UpdateStatus::up_to_date);

    // /releases/latest (one object) and an empty list.
    CHECK(ParseReleasesFeed(Release("v1.0.0", Asset("StrandedTogether-1.0.0.zip", GitHubUrl("v1.0.0", "StrandedTogether-1.0.0.zip"))), "0.9.0", u,
                            error) &&
          u.status == UpdateStatus::available && u.latest_version == "1.0.0" && u.asset_sha256.empty());
    CHECK(ParseReleasesFeed("[]", "0.1.0", u, error) && u.status == UpdateStatus::up_to_date && u.latest_version.empty());
    // A release with only a source zip, or with an asset not yet uploaded: nothing to offer.
    CHECK(ParseReleasesFeed("[" + Release("v2.0.0", Asset("StrandedTogether-2.0.0-source.zip", GitHubUrl("v2.0.0", "StrandedTogether-2.0.0-source.zip"))) + "]",
                            "0.1.0", u, error) &&
          u.status == UpdateStatus::up_to_date);
    std::string pending = Release("v2.0.0", Asset("StrandedTogether-2.0.0.zip", GitHubUrl("v2.0.0", "StrandedTogether-2.0.0.zip")));
    pending.replace(pending.find("\"uploaded\""), 10, "\"starter\"");
    CHECK(ParseReleasesFeed("[" + pending + "]", "0.1.0", u, error) && u.status == UpdateStatus::up_to_date);
    // Wrong shapes.
    CHECK(!ParseReleasesFeed("not json", "0.1.0", u, error) && !error.empty());
    CHECK(!ParseReleasesFeed("{\"message\":\"API rate limit exceeded\"}", "0.1.0", u, error));
    CHECK(!ParseReleasesFeed("42", "0.1.0", u, error));
    // An asset URL with a query or a different path never matches.
    CHECK(ParseReleasesFeed("[" + Release("v3.0.0", Asset("StrandedTogether-3.0.0.zip", GitHubUrl("v3.0.0", "StrandedTogether-3.0.0.zip") + "?x=1")) + "]",
                            "0.1.0", u, error) &&
          u.status == UpdateStatus::up_to_date);

    // A local test feed: only loopback download URLs.
    FeedOptions local;
    local.local_feed = true;
    const std::string local_feed = "[" + Release("v0.9.0", Asset("StrandedTogether-0.9.0.zip", "http://127.0.0.1:8765/StrandedTogether-0.9.0.zip")) + "," +
                                   Release("v0.9.5", Asset("StrandedTogether-0.9.5.zip", GitHubUrl("v0.9.5", "StrandedTogether-0.9.5.zip"))) + "]";
    CHECK(ParseReleasesFeed(local_feed, "0.1.0", u, error, local) && u.latest_version == "0.9.0");
    CHECK(ParseReleasesFeed(local_feed, "0.1.0", u, error) && u.latest_version == "0.9.5");

    // html_url ("Release notes", opened with ShellExecute): only the project's release pages.
    {
        const std::string good = Release("v4.0.0", Asset("StrandedTogether-4.0.0.zip", GitHubUrl("v4.0.0", "StrandedTogether-4.0.0.zip")));
        auto with_page = [&](const std::string& page) {
            std::string r = good;
            const std::string from = "https://github.com/TrollTaylor/stranded-together/releases/tag/v4.0.0";
            r.replace(r.find(from), from.size(), page);
            return "[" + r + "]";
        };
        CHECK(ParseReleasesFeed("[" + good + "]", "0.1.0", u, error) && u.html_url == "https://github.com/TrollTaylor/stranded-together/releases/tag/v4.0.0");
        CHECK(ParseReleasesFeed(with_page("C:\\\\Windows\\\\System32\\\\calc.exe"), "0.1.0", u, error) && u.latest_version == "4.0.0" &&
              u.html_url.empty());
        CHECK(ParseReleasesFeed(with_page("https://evil.example/releases/tag/v4.0.0"), "0.1.0", u, error) && u.html_url.empty());
        CHECK(ParseReleasesFeed(with_page("https://github.com/TrollTaylor/stranded-together/releases/x\\\"y"), "0.1.0", u, error) &&
              u.html_url.empty());
        CHECK(ParseReleasesFeed(with_page("http://127.0.0.1:8765/notes"), "0.1.0", u, error) && u.html_url.empty());
        std::string local_page = Release("v4.0.0", Asset("StrandedTogether-4.0.0.zip", "http://127.0.0.1:8765/StrandedTogether-4.0.0.zip"));
        const std::string from = "https://github.com/TrollTaylor/stranded-together/releases/tag/v4.0.0";
        local_page.replace(local_page.find(from), from.size(), "http://127.0.0.1:8765/notes");
        CHECK(ParseReleasesFeed("[" + local_page + "]", "0.1.0", u, error, local) && u.latest_version == "4.0.0" &&
              u.html_url == "http://127.0.0.1:8765/notes");
    }

    CHECK(IsLoopbackUrl("http://127.0.0.1:8765/releases.json"));
    CHECK(IsLoopbackUrl("https://localhost/feed"));
    CHECK(IsLoopbackUrl("http://LOCALHOST:1/"));
    CHECK(!IsLoopbackUrl("http://127.0.0.1.evil.com/x"));
    CHECK(!IsLoopbackUrl("http://user@127.0.0.1/x"));
    CHECK(!IsLoopbackUrl("http://evil.com/?127.0.0.1"));
    CHECK(!IsLoopbackUrl("ftp://127.0.0.1/x"));
    CHECK(!IsLoopbackUrl("http://127.0.0.1:abc/x"));
}

// ---- Verifying a zip ----------------------------------------------------------------------------
void VerifyTests() {
    const std::wstring dir = NewTempDir(L"st-update-verify-");
    const std::wstring zip = JoinPath2(dir, L"update.zip");
    ZipCheck check;
    std::string error;
    auto verify = [&](const std::string& bytes, const std::string& version) {
        PutFile(zip, bytes);
        error.clear();
        return VerifyUpdateZip(zip, version, check, error);
    };
    CHECK(verify(MakeReleaseZip("0.2.0", StandardFiles("new")), "0.2.0"));
    CHECK(check.version == "0.2.0" && check.build_id == "0.2.0+abc1234" && check.files.size() == 5);
    CHECK(verify(MakeReleaseZip("0.2.0", StandardFiles("new")), "v0.2.0"));   // the tag form

    CHECK(!verify(MakeReleaseZip("0.1.9", StandardFiles("new")), "0.2.0") && error.find("version") != std::string::npos);
    CHECK(!verify(MakeReleaseZip("0.2.0", StandardFiles("new"), false), "0.2.0"));
    CHECK(!verify(MakeReleaseZip("0.2.0", StandardFiles("new"), true, "something-else"), "0.2.0"));
    CHECK(!verify("PK not a zip at all", "0.2.0") && error.find("zip") != std::string::npos);
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f.erase(f.begin() + 1);   // no winhttp.dll
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0"));
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f[2].listed_sha = std::string(64, '0');   // README's hash is wrong
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0") && error.find("SHA-256") != std::string::npos);
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f.push_back({"stranded_together/extra.txt", "x", false});   // not in package.json
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0") && error.find("not in its package.json") != std::string::npos);
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f.push_back({"ds.exe", "MZ replaced game"});   // never a game file
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0"));
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f.push_back({"stranded_together/session.ini", "role=host\n"});   // never a runtime file
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0"));
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f.push_back({"steam_api64.dll", "MZ"});
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0"));
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f[1].data = "MZ another loader's winhttp.dll";   // not the mod
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0") && error.find("winhttp.dll") != std::string::npos);
    }
    {
        std::vector<ZipFile> f = StandardFiles("new");
        f[0].data = "#!/bin/sh";   // not a PE
        CHECK(!verify(MakeReleaseZip("0.2.0", f), "0.2.0"));
    }
    RemoveTree(dir);
}

// ---- Installing ---------------------------------------------------------------------------------
void ApplyTests() {
    std::string error;
    ApplyReport report;
    // 1. A normal update over 0.1.0.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        ApplyOptions o;
        int started = 0;
        o.game_running = [] { return false; };
        o.start_new = [&](std::string&) {
            ++started;
            // While the new launcher starts, the new files are in place and the old exe is aside.
            return ReadAll(paths.launcher_exe) == FakeExe("new") && ReadAll(paths.launcher_exe + L".old") == FakeExe("old");
        };
        CHECK(ApplyUpdate(paths, zip, "0.2.0", o, report, error));
        CHECK(started == 1 && report.files == 5 && report.version == "0.2.0" && report.failure == ApplyFailure::none);
        CHECK(ReadAll(paths.launcher_exe) == FakeExe("new") && ReadAll(paths.mod_dll) == FakeOurDll("new"));
        CHECK(ReadAll(JoinPath2(paths.mod, L"README.txt")) == "readme new");
        CHECK(ReadAll(paths.package_json).find("\"version\":\"0.2.0\"") != std::string::npos);
        CHECK(ReadAll(paths.settings) == "{\"transport\":\"steam\"}");   // the player's settings are kept
        CHECK(ReadAll(paths.exe) == "ds");
        CHECK(!IsFile(zip) && NoStagingLeft(paths));
        CHECK(IsFile(paths.launcher_exe + L".old"));   // the new launcher deletes it when it starts
        CHECK(CleanOldExecutable(paths) && !IsFile(paths.launcher_exe + L".old"));
        RemoveTree(dir);
    }
    // 2. ds.exe runs: refused, nothing touched.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        ApplyOptions o;
        o.game_running = [] { return true; };
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", o, report, error) && report.failure == ApplyFailure::game_running);
        CHECK(ReadAll(paths.launcher_exe) == FakeExe("old") && ReadAll(paths.mod_dll) == FakeOurDll("old") && NoStagingLeft(paths));
        CHECK(!ApplyFailureSentence(report).empty());
        // The game starts after staging, just before the commit: still nothing replaced.
        int calls = 0;
        o.game_running = [&] { return ++calls > 1; };
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", o, report, error) && report.failure == ApplyFailure::game_running);
        CHECK(ReadAll(paths.launcher_exe) == FakeExe("old") && NoStagingLeft(paths));
        RemoveTree(dir);
    }
    // 3. Another program's winhttp.dll: never replaced.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        PutFile(paths.mod_dll, "MZ another loader");
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", ApplyOptions{}, report, error) && report.failure == ApplyFailure::foreign_dll);
        CHECK(ReadAll(paths.mod_dll) == "MZ another loader" && ReadAll(paths.launcher_exe) == FakeExe("old") && NoStagingLeft(paths));
        RemoveTree(dir);
    }
    // 4. A damaged or wrong zip: refused before anything is written.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.3.0", StandardFiles("new")));
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", ApplyOptions{}, report, error) && report.failure == ApplyFailure::damaged);
        CHECK(ReadAll(paths.launcher_exe) == FakeExe("old") && NoStagingLeft(paths));
        RemoveTree(dir);
    }
    // 5. A move fails half way (the third file): everything goes back.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        const std::string old_package = ReadAll(paths.package_json);
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        ApplyOptions o;
        int commits = 0;
        o.before_commit = [&](const std::string&) { return ++commits < 3; };
        o.start_new = [](std::string&) { return true; };
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", o, report, error) && report.failure == ApplyFailure::install_failed && report.restored);
        CHECK(ReadAll(paths.launcher_exe) == FakeExe("old") && ReadAll(paths.mod_dll) == FakeOurDll("old"));
        CHECK(ReadAll(JoinPath2(paths.mod, L"README.txt")) == "readme old" && ReadAll(paths.package_json) == old_package);
        CHECK(!IsFile(paths.launcher_exe + L".old") && NoStagingLeft(paths));
        CHECK(ApplyFailureSentence(report).find("back in place") != std::string::npos);
        RemoveTree(dir);
    }
    // 6. The new launcher does not start: the update is undone.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        ApplyOptions o;
        o.start_new = [](std::string& e) {
            e = "it closed right after it started";
            return false;
        };
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", o, report, error) && report.failure == ApplyFailure::restart_failed && report.restored);
        CHECK(error == "it closed right after it started");
        CHECK(ReadAll(paths.launcher_exe) == FakeExe("old") && ReadAll(paths.mod_dll) == FakeOurDll("old"));
        CHECK(ReadAll(JoinPath2(paths.mod, L"THIRD_PARTY_NOTICES.txt")) == "notices old");
        CHECK(!IsFile(paths.launcher_exe + L".old") && NoStagingLeft(paths));
        RemoveTree(dir);
    }
    // 7. Files the folder did not have before are removed again on rollback; a fresh folder installs.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = GetInstancePaths(dir);
        PutFile(paths.exe, "ds");
        const std::wstring zip = JoinPath2(dir, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        ApplyOptions o;
        o.start_new = [](std::string&) { return false; };
        CHECK(!ApplyUpdate(paths, zip, "0.2.0", o, report, error) && report.restored);
        CHECK(!IsFile(paths.launcher_exe) && !IsFile(paths.mod_dll) && !IsFile(paths.package_json) && NoStagingLeft(paths));
        CHECK(ApplyUpdate(paths, zip, "0.2.0", ApplyOptions{}, report, error) && IsFile(paths.mod_dll) && IsFile(paths.package_json));
        RemoveTree(dir);
    }
    // 8. The mod is switched off (only winhttp.dll.disabled): it stays off, with the new DLL.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        CHECK(MoveFileExW(paths.mod_dll.c_str(), paths.mod_dll_disabled.c_str(), 0));
        const std::wstring zip = JoinPath2(paths.mod, L"update.zip");
        PutFile(zip, MakeReleaseZip("0.2.0", StandardFiles("new")));
        CHECK(ApplyUpdate(paths, zip, "0.2.0", ApplyOptions{}, report, error));
        CHECK(!IsFile(paths.mod_dll) && ReadAll(paths.mod_dll_disabled) == FakeOurDll("new"));
        RemoveTree(dir);
    }
    // 9. Leftovers of an update that was ended half way.
    {
        const std::wstring dir = NewTempDir(L"st-update-apply-");
        const InstancePaths paths = MakeGameFolder(dir, "old");
        PutFile(paths.mod_dll + L".st-new", "half written");
        CHECK(MoveFileExW(JoinPath2(paths.mod, L"README.txt").c_str(), JoinPath2(paths.mod, L"README.txt.st-old").c_str(), 0));
        PutFile(paths.launcher_exe + L".st-old", "old copy of an existing file");
        CHECK(CleanUpdateLeftovers(paths) == 3);
        CHECK(ReadAll(JoinPath2(paths.mod, L"README.txt")) == "readme old" && ReadAll(paths.launcher_exe) == FakeExe("old") && NoStagingLeft(paths));
        // An old exe that is still open (the old launcher exiting) is kept until it closes.
        PutFile(paths.launcher_exe + L".old", "running");
        HANDLE h = CreateFileW((paths.launcher_exe + L".old").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        CHECK(!CleanOldExecutable(paths, 0));
        CloseHandle(h);
        CHECK(CleanOldExecutable(paths, 0));
        RemoveTree(dir);
    }
}

// ---- The real model on a fake feed ----------------------------------------------------------------
void ModelTests() {
    const std::wstring dir = NewTempDir(L"st-update-model-");
    const InstancePaths paths = MakeGameFolder(dir, "old");
    DeleteFileChecked(paths.package_json);   // a development build: the model is ready
    auto env = std::make_shared<FakeEnv>();
    auto http = std::make_shared<FakeHttp>();
    const std::string zip = MakeReleaseZip("9.9.9", StandardFiles("new"));
    const std::string zip_url = GitHubUrl("v9.9.9", "StrandedTogether-9.9.9.zip");
    http->pages[kReleasesUrl] = "[" + Release("v9.9.9", Asset("StrandedTogether-9.9.9.zip", zip_url, zip.size(), "sha256:" + Sha256Hex(zip))) + "]";
    http->files[zip_url] = zip;
    std::vector<std::wstring> relaunched;
    UpdateSeams seams;
    seams.http = http;
    seams.synchronous = true;
    seams.relaunch = [&](const std::wstring& exe, const std::vector<std::wstring>& args, std::string&) {
        relaunched = args;
        return exe == paths.launcher_exe && ReadAll(exe) == FakeExe("new");
    };
    std::string error;

    // No check on start without check_on_start (tests, --selftest); the setting turns it off too.
    {
        auto m = MakeRealModelWithEnv(dir, env, ModelStart{}, seams);
        CHECK(m->phase() == Phase::ready);
        m->tick();
        CHECK(http->requested.empty() && m->update_info().status == UpdateStatus::idle && m->update_info().auto_check);
        m->set_auto_update_check(false);
        CHECK(ReadAll(paths.settings).find("\"auto_update_check\": false") != std::string::npos);
    }
    ModelStart start;
    start.check_on_start = true;
    {
        auto m = MakeRealModelWithEnv(dir, env, start, seams);
        CHECK(!m->update_info().auto_check);
        m->tick();
        m->tick();
        CHECK(http->requested.empty());
        m->set_auto_update_check(true);
    }
    // Once per launch.
    auto m = MakeRealModelWithEnv(dir, env, start, seams);
    m->tick();
    m->tick();
    CHECK(http->requested.size() == 1 && http->requested[0] == kReleasesUrl);
    UpdateInfo u = m->update_info();
    CHECK(u.status == UpdateStatus::available && u.latest_version == "9.9.9" && u.download_url == zip_url);
    CHECK(ui::UpdateBannerFor(u) == ui::UpdateBanner::available && ui::CanStartUpdate(m->phase(), u));

    // Update now while the game runs: refused with a sentence, nothing downloaded.
    env->game_running = true;
    env->game.pid = 9;
    env->game.path = paths.exe;
    m->start_update();
    u = m->update_info();
    CHECK(u.status == UpdateStatus::error && u.error.find("Quit DEATH STRANDING") != std::string::npos && http->requested.size() == 1);
    CHECK(ui::UpdateBannerFor(u) == ui::UpdateBanner::failed);
    env->game_running = false;

    // A download that does not match the feed's digest: refused, nothing changed.
    http->files[zip_url] = MakeReleaseZip("9.9.9", StandardFiles("wen"));
    m->start_update();
    u = m->update_info();
    CHECK(u.status == UpdateStatus::error && u.error.find("damaged") != std::string::npos);
    CHECK(ReadAll(paths.launcher_exe) == FakeExe("old") && !IsFile(JoinPath2(paths.mod, L"update.zip")));
    http->files[zip_url] = zip;

    // The download fails.
    http->offline = true;
    m->start_update();
    CHECK(m->update_info().status == UpdateStatus::error && m->update_info().error.find("downloaded") != std::string::npos);
    m->check_for_updates();
    CHECK(m->update_info().status == UpdateStatus::error && m->update_info().error_after_check);
    CHECK(ui::UpdateBannerFor(m->update_info()) == ui::UpdateBanner::none && !ui::CanStartUpdate(m->phase(), m->update_info()));
    http->offline = false;
    m->check_for_updates();
    CHECK(m->update_info().status == UpdateStatus::available);

    // The update goes through: the files are new, the new launcher is started with --after-update <pid>.
    m->start_update();
    u = m->update_info();
    CHECK(u.status == UpdateStatus::ready_to_restart && ui::CloseForUpdate(u));
    CHECK(relaunched.size() == 2 && relaunched[0] == L"--after-update" && relaunched[1] == L"4242");
    CHECK(ReadAll(paths.launcher_exe) == FakeExe("new") && ReadAll(paths.mod_dll) == FakeOurDll("new"));
    CHECK(IsFile(paths.launcher_exe + L".old") && !IsFile(JoinPath2(paths.mod, L"update.zip")) && NoStagingLeft(paths));
    // While an update is installed (or handed over), nothing starts the game or switches the mod.
    error.clear();
    CHECK(!m->solo_test(error) && error.find("update") != std::string::npos);
    error.clear();
    CHECK(!m->set_mod_enabled(false, error) && error.find("update") != std::string::npos && IsFile(paths.mod_dll));
    m.reset();

    // The new launcher: deletes the .old exe and says "Updated to".
    ModelStart after;
    after.after_update = 4242;
    {
        auto b = MakeRealModelWithEnv(dir, env, after, seams);
        CHECK(!IsFile(paths.launcher_exe + L".old"));
        u = b->update_info();
        CHECK(!u.updated_to.empty() && ui::UpdateBannerFor(u) == ui::UpdateBanner::updated);
    }

    // A failed restart rolls back (the relaunch seam refuses).
    {
        auto c = MakeRealModelWithEnv(dir, env, ModelStart{}, [&] {
            UpdateSeams s = seams;
            s.relaunch = [](const std::wstring&, const std::vector<std::wstring>&, std::string& e) {
                e = "no";
                return false;
            };
            return s;
        }());
        const std::string before = ReadAll(paths.launcher_exe);
        c->check_for_updates();
        CHECK(c->update_info().status == UpdateStatus::available);
        c->start_update();
        u = c->update_info();
        CHECK(u.status == UpdateStatus::error && u.error.find("put back") != std::string::npos);
        CHECK(ReadAll(paths.launcher_exe) == before && NoStagingLeft(paths) && !IsFile(paths.launcher_exe + L".old"));
    }

    // A developer feed is used (with its loopback download), and passed on to the new launcher.
    {
        ModelStart dev;
        dev.update_feed = "http://127.0.0.1:8765/releases.json";
        auto local_http = std::make_shared<FakeHttp>();
        const std::string local_url = "http://127.0.0.1:8765/StrandedTogether-9.9.9.zip";
        local_http->pages[dev.update_feed] = "[" + Release("v9.9.9", Asset("StrandedTogether-9.9.9.zip", local_url, zip.size())) + "]";
        local_http->files[local_url] = zip;
        UpdateSeams s = seams;
        s.http = local_http;
        auto d = MakeRealModelWithEnv(dir, env, dev, s);
        d->check_for_updates();
        CHECK(d->update_info().status == UpdateStatus::available && d->update_info().download_url == local_url);
        d->start_update();
        CHECK(d->update_info().status == UpdateStatus::ready_to_restart);
        CHECK(relaunched.size() == 4 && relaunched[2] == L"--update-feed" && relaunched[3] == L"http://127.0.0.1:8765/releases.json");
        // --update-now: the check on start finds it and installs it in the same tick, once.
        dev.check_on_start = true;
        dev.update_now = true;
        relaunched.clear();
        auto now = MakeRealModelWithEnv(dir, env, dev, s);
        now->tick();
        CHECK(now->update_info().status == UpdateStatus::ready_to_restart && relaunched.size() == 4);
        // A feed that is not loopback is ignored (GitHub's is read instead).
        ModelStart bad;
        bad.update_feed = "http://example.com/releases.json";
        auto e = MakeRealModelWithEnv(dir, env, bad, seams);
        http->requested.clear();
        e->check_for_updates();
        CHECK(http->requested.size() == 1 && http->requested[0] == kReleasesUrl);
    }
    CleanOldExecutable(paths, 0);
    RemoveTree(dir);
}

// ---- UI logic, command line, mock -------------------------------------------------------------------
void UiTests() {
    UpdateInfo u;
    CHECK(ui::UpdateBannerFor(u) == ui::UpdateBanner::none && ui::CanCheckForUpdates(u) && !ui::CanStartUpdate(Phase::ready, u));
    u = MockModel::MockRelease();
    CHECK(ui::UpdateBannerFor(u) == ui::UpdateBanner::available && ui::CanStartUpdate(Phase::ready, u) && ui::CanStartUpdate(Phase::error, u));
    CHECK(!ui::CanStartUpdate(Phase::in_game, u) && !ui::CanStartUpdate(Phase::launching, u) && !ui::CanStartUpdate(Phase::not_in_game_folder, u));
    CHECK(ui::UpdateStatusText(u) == "Version 0.2.0 is available.");
    u.status = UpdateStatus::downloading;
    CHECK(ui::UpdateBannerFor(u) == ui::UpdateBanner::downloading && ui::UpdateInProgress(u) && !ui::CanCheckForUpdates(u) &&
          !ui::CanStartUpdate(Phase::ready, u));
    u.status = UpdateStatus::ready_to_restart;
    CHECK(ui::CloseForUpdate(u) && ui::UpdateBannerFor(u) == ui::UpdateBanner::installing);
    u.status = UpdateStatus::checking;
    CHECK(!ui::CanCheckForUpdates(u) && ui::UpdateStatusText(u) == "Checking for updates...");
    UpdateInfo done;
    done.status = UpdateStatus::up_to_date;
    done.updated_to = "0.2.0";
    CHECK(ui::UpdateBannerFor(done) == ui::UpdateBanner::updated && ui::UpdateStatusText(done).find("0.2.0") != std::string::npos);

    CHECK(ui::NotesExcerpt("## What's new\r\n\r\n* **Animations** for the partner\n- Fixed `code`\n- third\n", 2, 200) ==
          "- Animations for the partner\n- Fixed code...");
    CHECK(ui::NotesExcerpt("One sentence.\nTwo.", 1, 200) == "One sentence...");
    CHECK(ui::NotesExcerpt("- Fixed `stranded_together\\session.ini`", 3, 200) == "- Fixed stranded_together\\session.ini");
    CHECK(ui::NotesExcerpt("", 3, 100).empty());
    CHECK(ui::NotesExcerpt("one\ntwo", 5, 100) == "one\ntwo");
    CHECK(ui::NotesExcerpt("abcdefghij", 5, 4) == "abcd...");
    // The cut never splits a UTF-8 character ("é" is 2 bytes, "—" 3).
    CHECK(ui::NotesExcerpt("abc\xC3\xA9xyz", 5, 4) == "abc...");
    CHECK(ui::NotesExcerpt("ab\xE2\x80\x94zz", 5, 4) == "ab...");
    CHECK(ui::FormatBytes(512) == "512 bytes" && ui::FormatBytes(2048) == "2 KB" && ui::FormatBytes(1835008) == "1.8 MB");

    ui::Options o = ui::ParseCommandLine({L"--update-feed", L"http://127.0.0.1:8765/releases.json"});
    CHECK(o.error.empty() && o.update_feed == L"http://127.0.0.1:8765/releases.json");
    o = ui::ParseCommandLine({L"--update-feed", L"https://api.github.com/repos/x/y/releases"});
    CHECK(!o.error.empty());
    o = ui::ParseCommandLine({L"--after-update", L"1234"});
    CHECK(o.error.empty() && o.after_update == 1234);
    CHECK(!ui::ParseCommandLine({L"--after-update", L"abc"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--after-update", L"0"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--update-now"}).error.empty());
    o = ui::ParseCommandLine({L"--update-feed", L"http://localhost:1/x", L"--update-now"});
    CHECK(o.error.empty() && o.update_now);
    CHECK(!ui::ParseCommandLine({L"--mock", L"--update-feed", L"http://localhost:1/x"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--selftest", L"--after-update", L"5"}).error.empty());
    CHECK(std::wstring(ui::UsageText()).find(L"--update-feed") != std::wstring::npos);

    std::set<std::string> names;
    for (int i = 0; i < ui::SceneCount(); ++i) names.insert(ui::SceneName(static_cast<ui::Scene>(i)));
    CHECK(static_cast<int>(names.size()) == ui::SceneCount() && names.count("31-play-update-available") && names.count("35-about-updates") &&
          !names.count(""));

    // The mock: Check finds a release, Update now downloads, installs and reports "Updated to".
    MockModel mock(MockOptions{});
    mock.use_manual_clock(0.0);
    CHECK(mock.update_info().status == UpdateStatus::idle);
    mock.check_for_updates();
    mock.advance(2.0);
    mock.tick();
    CHECK(mock.update_info().status == UpdateStatus::available);
    mock.start_update();
    mock.advance(2.0);
    mock.tick();
    CHECK(mock.update_info().status == UpdateStatus::downloading && mock.update_info().progress > 0.4f);
    mock.advance(2.5);
    mock.tick();
    CHECK(mock.update_info().status == UpdateStatus::applying);
    mock.advance(2.0);
    mock.tick();
    CHECK(mock.update_info().status == UpdateStatus::up_to_date && mock.update_info().updated_to == "0.2.0");
    MockScenario s;
    CHECK(ParseMockScenario("update-available", s) && s == MockScenario::update_available);
    MockOptions with;
    with.scenario = MockScenario::update_available;
    MockModel offered(with);
    CHECK(offered.update_info().status == UpdateStatus::available);
    std::string error, code;
    CHECK(offered.host(code, error));
    offered.start_update();
    CHECK(offered.update_info().status == UpdateStatus::error);   // not during a session
}

void WinHttpLoads() {
    // System32's winhttp.dll (never this folder's), and nothing else named winhttp.dll is in the process.
    std::wstring from;
    std::string error;
    CHECK(LoadSystemWinHttp(&from, &error));
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    CHECK(IEquals(from, JoinPath2(sys, L"winhttp.dll")));
    HMODULE loaded = GetModuleHandleW(L"winhttp.dll");
    wchar_t path[MAX_PATH]{};
    CHECK(loaded && GetModuleFileNameW(loaded, path, MAX_PATH) && IEquals(path, from));
}

// The real restart handoff (StartSuccessor) with this exe as the "new launcher":
//   --handoff-signal <pid>  sets the handoff event of <pid> and exits (a launcher that came up)
//   --handoff-exit          exits at once with code 3 (a launcher that failed to start)
//   --handoff-hang          sleeps a minute (a hung launcher: StartSuccessor must end it)
void HandoffTests() {
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    std::string error;
    CHECK(StartSuccessor(self, {L"--handoff-signal", pid}, L"", 20000, error));
    error.clear();
    CHECK(!StartSuccessor(self, {L"--handoff-exit"}, L"", 20000, error) && error.find("exit code 3") != std::string::npos);
    error.clear();
    const ULONGLONG t0 = GetTickCount64();
    CHECK(!StartSuccessor(self, {L"--handoff-hang"}, L"", 1500, error) && error.find("did not answer") != std::string::npos);
    CHECK(GetTickCount64() - t0 < 15000);   // ended, not waited for
}

// The real WinHTTP client against a tiny HTTP server on 127.0.0.1 (one thread, Connection: close).
class LoopbackServer {
public:
    std::map<std::string, std::string> pages;   // path -> body (anything else: 404)
    int port = 0;
    std::vector<std::string> seen;

    bool Start(int requests) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = sizeof(a);
        if (listener_ == INVALID_SOCKET || bind(listener_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(listener_, 4) != 0 ||
            getsockname(listener_, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return false;
        port = ntohs(a.sin_port);
        thread_ = std::thread([this, requests] {
            for (int i = 0; i < requests; ++i) {
                SOCKET c = accept(listener_, nullptr, nullptr);
                if (c == INVALID_SOCKET) return;
                std::string req;
                char buf[2048];
                while (req.find("\r\n\r\n") == std::string::npos) {
                    const int n = recv(c, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    req.append(buf, static_cast<std::size_t>(n));
                }
                const std::size_t sp = req.find(' '), sp2 = req.find(' ', sp + 1);
                const std::string path = sp == std::string::npos || sp2 == std::string::npos ? std::string() : req.substr(sp + 1, sp2 - sp - 1);
                seen.push_back(req.substr(0, req.find("\r\n\r\n")));
                auto it = pages.find(path);
                const std::string body = it == pages.end() ? std::string("not found") : it->second;
                const std::string head = std::string(it == pages.end() ? "HTTP/1.1 404 Not Found" : "HTTP/1.1 200 OK") +
                                         "\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(body.size()) +
                                         "\r\nConnection: close\r\n\r\n";
                const std::string all = head + body;
                send(c, all.data(), static_cast<int>(all.size()), 0);
                shutdown(c, SD_SEND);
                closesocket(c);
            }
        });
        return true;
    }
    ~LoopbackServer() {
        if (listener_ != INVALID_SOCKET) closesocket(listener_);
        if (thread_.joinable()) thread_.join();
        WSACleanup();
    }

private:
    SOCKET listener_ = INVALID_SOCKET;
    std::thread thread_;
};

void WinHttpLoopback() {
    LoopbackServer server;
    std::string big(300000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
    server.pages["/releases.json"] = "[{\"tag_name\":\"v0.2.0\"}]";
    server.pages["/StrandedTogether-0.2.0.zip"] = big;
    if (!server.Start(4)) {
        CHECK(!"loopback server");
        return;
    }
    const std::string base = "http://127.0.0.1:" + std::to_string(server.port);
    auto http = MakeWinHttpClient("StrandedTogether-Launcher/test");
    std::string body, error;
    CHECK(http->Get(base + "/releases.json", kMaxFeedBytes, body, error) && body == "[{\"tag_name\":\"v0.2.0\"}]");
    CHECK(!http->Get(base + "/missing", kMaxFeedBytes, body, error) && error.find("404") != std::string::npos);
    const std::wstring dir = NewTempDir(L"st-update-http-");
    const std::wstring dest = JoinPath2(dir, L"update.zip");
    std::uint64_t last = 0, total = 0;
    CHECK(http->Download(base + "/StrandedTogether-0.2.0.zip", dest, kMaxUpdateZipBytes, [&](const HttpProgress& p) {
        last = p.bytes;
        total = p.total;
    }, nullptr, error));
    CHECK(ReadAll(dest) == big && last == big.size() && total == big.size());
    // Larger than allowed: refused and nothing left behind.
    CHECK(!http->Download(base + "/StrandedTogether-0.2.0.zip", dest, 1000, nullptr, nullptr, error) && !IsFile(dest));
    CHECK(server.seen.size() == 4 && server.seen[0].find("User-Agent: StrandedTogether-Launcher/test") != std::string::npos);
    RemoveTree(dir);
}

// ---- --check-imports <exe> ----------------------------------------------------------------------------
int CheckImports(const std::wstring& exe) {
    Bytes data;
    std::string error;
    if (!ReadFileShared(exe, data, 256u << 20, error) || data.size() < sizeof(IMAGE_DOS_HEADER)) {
        std::printf("FAIL: cannot read %s\n", Utf8(exe).c_str());
        return 1;
    }
    auto at = [&](std::size_t off, std::size_t size) { return off + size <= data.size() ? data.data() + off : nullptr; };
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(at(static_cast<std::size_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64)));
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || !nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        std::printf("FAIL: not a PE32+ image\n");
        return 1;
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(
        reinterpret_cast<const std::uint8_t*>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);
    auto offset_of = [&](DWORD rva) -> std::size_t {
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            const IMAGE_SECTION_HEADER& s = sections[i];
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData))
                return static_cast<std::size_t>(rva - s.VirtualAddress + s.PointerToRawData);
        }
        return static_cast<std::size_t>(-1);
    };
    auto name_at = [&](DWORD rva) {
        std::string n;
        for (std::size_t off = offset_of(rva); off < data.size() && data[off]; ++off) n.push_back(static_cast<char>(data[off]));
        return n;
    };
    std::vector<std::string> dlls;
    const IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (std::size_t off = offset_of(imp.VirtualAddress); imp.VirtualAddress && off != static_cast<std::size_t>(-1); off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(at(off, sizeof(IMAGE_IMPORT_DESCRIPTOR)));
        if (!d || d->Name == 0) break;
        dlls.push_back(name_at(d->Name));
    }
    const IMAGE_DATA_DIRECTORY& delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    for (std::size_t off = offset_of(delay.VirtualAddress); delay.VirtualAddress && off != static_cast<std::size_t>(-1);
         off += sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
        const auto* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(at(off, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)));
        if (!d || d->DllNameRVA == 0) break;
        dlls.push_back(name_at(d->DllNameRVA) + " (delay)");
    }
    int failures = 0;
    std::printf("imports:");
    for (const std::string& d : dlls) {
        std::printf(" %s", d.c_str());
        if (StartsWith(LowerAscii(d), "winhttp")) ++failures;
    }
    std::printf("\n");
    if (dlls.empty()) {
        std::printf("FAIL: no import table found\n");
        ++failures;
    }
    if (failures && !dlls.empty()) std::printf("FAIL: StrandedTogether.exe imports winhttp.dll (the mod's proxy would load into it)\n");
    const IMAGE_DATA_DIRECTORY& lc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    const std::size_t lc_off = offset_of(lc.VirtualAddress);
    const std::size_t field = offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DependentLoadFlags);
    const std::uint8_t* config = lc.VirtualAddress && lc_off != static_cast<std::size_t>(-1) ? at(lc_off, field + sizeof(WORD)) : nullptr;
    DWORD config_size = 0;
    if (config) std::memcpy(&config_size, config, sizeof(config_size));
    WORD flags = 0;
    if (config && config_size >= field + sizeof(WORD)) std::memcpy(&flags, config + field, sizeof(flags));
    std::printf("DependentLoadFlags: 0x%04X\n", flags);
    if (flags != LOAD_LIBRARY_SEARCH_SYSTEM32) {
        std::printf("FAIL: /DEPENDENTLOADFLAG:0x800 is missing\n");
        ++failures;
    }
    std::printf("import check: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--check-imports") return CheckImports(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--handoff-signal") {
        const HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, UpdateHandoffEventName(static_cast<std::uint32_t>(_wtoi(argv[2]))).c_str());
        const bool ok = ev && SetEvent(ev);
        if (ev) CloseHandle(ev);
        return ok ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--handoff-exit") return 3;
    if (argc == 2 && std::wstring(argv[1]) == L"--handoff-hang") {
        Sleep(60000);
        return 0;
    }
    if (argc == 4 && std::wstring(argv[1]) == L"--verify-zip") {
        // The launcher's own check of a release zip (tools/release.ps1 runs it before publishing).
        ZipCheck check;
        std::string error;
        const bool ok = VerifyUpdateZip(argv[2], Utf8(argv[3]), check, error);
        if (ok) std::printf("verify-zip: ok, version %s, build %s, %zu files\n", check.version.c_str(), check.build_id.c_str(), check.files.size());
        else std::printf("verify-zip: FAIL: the zip %s\n", error.c_str());
        return ok ? 0 : 1;
    }
    if (argc >= 2 && std::wstring(argv[1]) == L"--live-feed") {
        const std::string current = argc >= 3 ? Utf8(argv[2]) : "0.0.1";
        std::string body, error;
        UpdateInfo u;
        const bool ok = MakeWinHttpClient("StrandedTogether-Launcher/test")->Get(kReleasesUrl, kMaxFeedBytes, body, error) &&
                        ParseReleasesFeed(body, current, u, error);
        std::printf("feed %s: %s (%zu bytes)\n", kReleasesUrl, ok ? "ok" : error.c_str(), body.size());
        if (ok)
            std::printf("current %s -> status %s, latest '%s', asset '%s', sha256 '%s'\n", current.c_str(),
                        u.status == UpdateStatus::available ? "available" : "up_to_date", u.latest_version.c_str(), u.asset_name.c_str(),
                        u.asset_sha256.c_str());
        return ok ? 0 : 1;
    }
    SemVerTests();
    FeedTests();
    VerifyTests();
    ApplyTests();
    ModelTests();
    UiTests();
    WinHttpLoads();
    WinHttpLoopback();
    HandoffTests();
    std::printf("launcher update tests: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
