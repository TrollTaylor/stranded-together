// Stranded Together launcher core: updates from GitHub Releases (see core_update.hpp).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Types and constants only: every WinHTTP function is taken from System32's
// winhttp.dll with GetProcAddress (WinHttpApi), never imported.
#include <winhttp.h>

#include "core_update.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>

#include "core_install.hpp"
#include "core_json.hpp"
#include "core_util.hpp"
#include "zip.hpp"

namespace st::core {

// ---- Versions ------------------------------------------------------------------------
namespace {
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsIdentChar(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-'; }
bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!IsDigit(c)) return false;
    return true;
}

bool ReadNumber(const std::string& s, std::size_t& i, std::uint32_t& out) {
    const std::size_t start = i;
    std::uint64_t v = 0;
    while (i < s.size() && IsDigit(s[i])) {
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
        if (v > 0xFFFFFFFFull) return false;
        ++i;
    }
    out = static_cast<std::uint32_t>(v);
    return i > start;
}

// Dot-separated identifiers from i up to (not including) a character in `stop` or the end.
bool ReadIdentifiers(const std::string& s, std::size_t& i, const char* stop, std::vector<std::string>* out) {
    std::string current;
    bool any = false;
    for (;; ++i) {
        const bool end = i >= s.size() || std::strchr(stop, s[i]) != nullptr;
        if (end || s[i] == '.') {
            if (current.empty()) return false;
            if (out) out->push_back(current);
            current.clear();
            any = true;
            if (end) return any;
            continue;
        }
        if (!IsIdentChar(s[i])) return false;
        current.push_back(s[i]);
    }
}
}  // namespace

bool ParseSemVer(const std::string& raw, SemVer& out) {
    out = SemVer{};
    const std::string s = Trim(raw);
    std::size_t i = 0;
    if (i < s.size() && (s[i] == 'v' || s[i] == 'V')) ++i;
    if (!ReadNumber(s, i, out.major) || i >= s.size() || s[i++] != '.') return false;
    if (!ReadNumber(s, i, out.minor) || i >= s.size() || s[i++] != '.') return false;
    if (!ReadNumber(s, i, out.patch)) return false;
    if (i < s.size() && s[i] == '-') {
        ++i;
        if (!ReadIdentifiers(s, i, "+", &out.pre)) return false;
    }
    if (i < s.size() && s[i] == '+') {
        ++i;
        if (!ReadIdentifiers(s, i, "", nullptr)) return false;
    }
    return i == s.size();
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;   // 1.0.0-rc < 1.0.0
    for (std::size_t k = 0; k < a.pre.size() && k < b.pre.size(); ++k) {
        const std::string& x = a.pre[k];
        const std::string& y = b.pre[k];
        const bool xn = AllDigits(x), yn = AllDigits(y);
        if (xn && yn) {
            // Numerically, without overflow: strip leading zeros, then length, then digits.
            const std::size_t xs = std::min(x.find_first_not_of('0'), x.size()), ys = std::min(y.find_first_not_of('0'), y.size());
            const std::string xv = x.substr(xs), yv = y.substr(ys);
            if (xv.size() != yv.size()) return xv.size() < yv.size() ? -1 : 1;
            if (xv != yv) return xv < yv ? -1 : 1;
            continue;
        }
        if (xn != yn) return xn ? -1 : 1;   // numeric identifiers are lower
        if (x != y) return x < y ? -1 : 1;
    }
    if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

bool IsVersionNewer(const std::string& candidate, const std::string& current) {
    SemVer a, b;
    return ParseSemVer(candidate, a) && ParseSemVer(current, b) && CompareSemVer(a, b) > 0;
}

std::string FormatSemVer(const SemVer& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    for (std::size_t k = 0; k < v.pre.size(); ++k) s += (k == 0 ? "-" : ".") + v.pre[k];
    return s;
}

std::string UpdateAssetName(const std::string& version) { return "StrandedTogether-" + version + ".zip"; }

// ---- The release feed ------------------------------------------------------------------
bool IsLoopbackUrl(const std::string& url) {
    const std::string lower = LowerAscii(url);
    std::string rest;
    if (StartsWith(lower, "http://")) rest = lower.substr(7);
    else if (StartsWith(lower, "https://")) rest = lower.substr(8);
    else return false;
    const std::size_t end = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, end);
    if (authority.find('@') != std::string::npos) return false;   // no user info
    const std::size_t colon = authority.rfind(':');
    std::string host = authority;
    if (colon != std::string::npos) {
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !AllDigits(port)) return false;
        host = authority.substr(0, colon);
    }
    return host == "127.0.0.1" || host == "localhost";
}

bool ParseReleasesFeed(const std::string& json_text, const std::string& current_version, UpdateInfo& out, std::string& error,
                       const FeedOptions& options) {
    json::Value root;
    std::string parse_error;
    if (!json::Parse(json_text, root, &parse_error)) {
        error = "the release feed is not JSON (" + parse_error + ")";
        return false;
    }
    std::vector<const json::Value*> releases;
    if (root.is_array()) {
        for (const json::Value& el : root.Elements())
            if (el.is_object()) releases.push_back(&el);
    } else if (root.is_object() && root.Find("tag_name")) {
        releases.push_back(&root);
    } else {
        error = "the release feed is neither a list of releases nor a release";
        return false;
    }

    out.current_version = current_version;
    out.status = UpdateStatus::up_to_date;
    out.latest_version.clear();
    out.tag.clear();
    out.release_title.clear();
    out.release_notes.clear();
    out.html_url.clear();
    out.asset_name.clear();
    out.download_url.clear();
    out.asset_sha256.clear();
    out.download_size = 0;
    out.error.clear();

    SemVer best_version;
    const json::Value* best = nullptr;
    const json::Value* best_asset = nullptr;
    for (const json::Value* rel : releases) {
        if (rel->Bool("draft", false)) continue;
        if (rel->Bool("prerelease", false) && !options.allow_prerelease) continue;
        const std::string tag = rel->Str("tag_name");
        SemVer v;
        if (!ParseSemVer(tag, v)) continue;
        if (!v.pre.empty() && !options.allow_prerelease) continue;   // a "-rc" tag published as a release
        const json::Value* assets = rel->Find("assets");
        if (!assets || !assets->is_array()) continue;
        const std::string want = UpdateAssetName(FormatSemVer(v));
        const json::Value* match = nullptr;
        int matches = 0;
        for (const json::Value& a : assets->Elements()) {
            if (!a.is_object() || a.Str("name") != want) continue;
            if (const json::Value* state = a.Find("state"); state && state->is_string() && state->StringRef() != "uploaded") continue;
            const std::string url = a.Str("browser_download_url");
            const bool url_ok = options.local_feed ? IsLoopbackUrl(url)
                                                   : StartsWith(url, kReleaseDownloadPrefix) && EndsWith(url, "/" + want) &&
                                                         url.find('?') == std::string::npos && url.find("..") == std::string::npos;
            if (!url_ok) continue;
            ++matches;
            match = &a;
        }
        if (matches != 1) continue;   // none, or an ambiguous release: never guess
        if (!best || CompareSemVer(v, best_version) > 0) {
            best = rel;
            best_asset = match;
            best_version = v;
        }
    }
    if (!best) return true;

    const std::string version = FormatSemVer(best_version);
    out.latest_version = version;
    out.tag = best->Str("tag_name");
    out.release_title = best->Str("name");
    if (out.release_title.empty()) out.release_title = "Stranded Together " + version;
    out.release_notes = best->Str("body");
    // Opened with ShellExecute by "Release notes": only the project's release page (or a
    // loopback page of a --update-feed), never a path or another program's URL.
    out.html_url = best->Str("html_url");
    const bool page_ok = out.html_url.find_first_of(" \"<>\\^`{|}") == std::string::npos &&
                         (StartsWith(out.html_url, kReleasePagePrefix) || (options.local_feed && IsLoopbackUrl(out.html_url)));
    if (!page_ok) out.html_url.clear();
    out.asset_name = best_asset->Str("name");
    out.download_url = best_asset->Str("browser_download_url");
    const std::int64_t size = best_asset->Int("size", 0);
    out.download_size = size > 0 ? static_cast<std::uint64_t>(size) : 0;
    const std::string digest = best_asset->Str("digest");
    if (StartsWith(LowerAscii(digest), "sha256:") && IsHexText(digest.substr(7), 64)) out.asset_sha256 = UpperAscii(digest.substr(7));
    out.status = IsVersionNewer(version, current_version) ? UpdateStatus::available : UpdateStatus::up_to_date;
    return true;
}

// ---- WinHTTP, loaded from System32 ---------------------------------------------------------
namespace {
struct WinHttpApi {
    HMODULE module = nullptr;
    std::wstring path;
    std::string error;
    decltype(&::WinHttpOpen) Open = nullptr;
    decltype(&::WinHttpConnect) Connect = nullptr;
    decltype(&::WinHttpOpenRequest) OpenRequest = nullptr;
    decltype(&::WinHttpSetOption) SetOption = nullptr;
    decltype(&::WinHttpSetTimeouts) SetTimeouts = nullptr;
    decltype(&::WinHttpSendRequest) SendRequest = nullptr;
    decltype(&::WinHttpReceiveResponse) ReceiveResponse = nullptr;
    decltype(&::WinHttpQueryHeaders) QueryHeaders = nullptr;
    decltype(&::WinHttpReadData) ReadData = nullptr;
    decltype(&::WinHttpCloseHandle) CloseHandle = nullptr;
    decltype(&::WinHttpCrackUrl) CrackUrl = nullptr;
};

std::wstring ModulePath(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring SystemDirectory() {
    wchar_t buf[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

template <typename T>
bool Bind(HMODULE module, const char* name, T& fn) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return fn != nullptr;
}

const WinHttpApi& Api() {
    static WinHttpApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        // Only System32: the game folder's winhttp.dll is the mod's proxy.
        HMODULE m = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) {
            api.error = "winhttp.dll could not be loaded from System32 (" + WinError(GetLastError()) + ")";
            return;
        }
        const std::wstring path = ModulePath(m);
        const std::wstring expected = JoinPath2(SystemDirectory(), L"winhttp.dll");
        if (path.empty() || expected.size() <= 12 || !IEquals(FullPath(path), FullPath(expected))) {
            // A module of that name was already loaded from elsewhere: never use it.
            FreeLibrary(m);
            api.error = "winhttp.dll in this process is not System32's";
            return;
        }
        const bool ok = Bind(m, "WinHttpOpen", api.Open) && Bind(m, "WinHttpConnect", api.Connect) &&
                        Bind(m, "WinHttpOpenRequest", api.OpenRequest) && Bind(m, "WinHttpSetOption", api.SetOption) &&
                        Bind(m, "WinHttpSetTimeouts", api.SetTimeouts) && Bind(m, "WinHttpSendRequest", api.SendRequest) &&
                        Bind(m, "WinHttpReceiveResponse", api.ReceiveResponse) && Bind(m, "WinHttpQueryHeaders", api.QueryHeaders) &&
                        Bind(m, "WinHttpReadData", api.ReadData) && Bind(m, "WinHttpCloseHandle", api.CloseHandle) &&
                        Bind(m, "WinHttpCrackUrl", api.CrackUrl);
        if (!ok) {
            FreeLibrary(m);
            api = WinHttpApi{};
            api.error = "System32's winhttp.dll lacks a function the launcher needs";
            return;
        }
        api.module = m;   // kept for the life of the process
        api.path = path;
    });
    return api;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h) Api().CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

class WinHttpClient final : public HttpClient {
public:
    explicit WinHttpClient(std::string user_agent) : agent_(Wide(user_agent)) {}

    bool Get(const std::string& url, std::uint64_t max_bytes, std::string& body, std::string& error) override {
        body.clear();
        return Request(url, L"Accept: application/vnd.github+json\r\n", [&](HINTERNET req, std::uint64_t, std::string& err) {
            std::vector<char> buf(16384);
            for (;;) {
                DWORD got = 0;
                if (!Api().ReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                    err = "the connection broke (" + WinError(GetLastError()) + ")";
                    return false;
                }
                if (got == 0) return true;
                if (body.size() + got > max_bytes) {
                    err = "the answer is too large";
                    return false;
                }
                body.append(buf.data(), got);
            }
        }, error);
    }

    bool Download(const std::string& url, const std::wstring& dest, std::uint64_t max_bytes,
                  const std::function<void(const HttpProgress&)>& on_progress, const std::atomic<bool>* cancel,
                  std::string& error) override {
        CreateDirectories(ParentOf(dest));
        const bool ok = Request(url, L"Accept: application/octet-stream\r\n", [&](HINTERNET req, std::uint64_t total, std::string& err) {
            if (total > max_bytes) {
                err = "the file is too large";
                return false;
            }
            HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                err = "the download could not be saved (" + WinError(GetLastError()) + ")";
                return false;
            }
            std::vector<char> buf(65536);
            std::uint64_t bytes = 0;
            bool good = true;
            for (;;) {
                if (cancel && cancel->load()) {
                    err = "cancelled";
                    good = false;
                    break;
                }
                DWORD got = 0;
                if (!Api().ReadData(req, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
                    err = "the connection broke (" + WinError(GetLastError()) + ")";
                    good = false;
                    break;
                }
                if (got == 0) break;
                if (bytes + got > max_bytes) {
                    err = "the file is too large";
                    good = false;
                    break;
                }
                DWORD written = 0;
                if (!WriteFile(file, buf.data(), got, &written, nullptr) || written != got) {
                    err = "the download could not be saved (" + WinError(GetLastError()) + ")";
                    good = false;
                    break;
                }
                bytes += got;
                if (on_progress) on_progress(HttpProgress{bytes, total});
            }
            if (good && !FlushFileBuffers(file)) {
                err = "the download could not be saved (" + WinError(GetLastError()) + ")";
                good = false;
            }
            ::CloseHandle(file);
            if (good && total && bytes != total) {
                err = "the download stopped early";
                good = false;
            }
            return good;
        }, error);
        if (!ok) DeleteFileW(dest.c_str());
        return ok;
    }

private:
    bool Request(const std::string& url, const wchar_t* accept,
                 const std::function<bool(HINTERNET, std::uint64_t total, std::string&)>& on_body, std::string& error) {
        const WinHttpApi& api = Api();
        if (!api.module) {
            error = api.error;
            return false;
        }
        const std::wstring wurl = Wide(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!api.CrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP)) {
            error = "not a web address";
            return false;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty()) path = L"/";
        Handle session(api.Open(agent_.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.h) session.h = api.Open(agent_.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.h) {
            error = "the network could not be used (" + WinError(GetLastError()) + ")";
            return false;
        }
        api.SetTimeouts(session.h, 10000, 10000, 15000, 30000);
        Handle connect(api.Connect(session.h, host.c_str(), parts.nPort, 0));
        if (!connect.h) {
            error = "the server could not be reached (" + WinError(GetLastError()) + ")";
            return false;
        }
        Handle request(api.OpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
        if (!request.h) {
            error = "the request could not be made (" + WinError(GetLastError()) + ")";
            return false;
        }
        // Redirects are followed (a release asset moves to GitHub's download host), never from https to http.
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        api.SetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        std::wstring headers = std::wstring(L"User-Agent: ") + agent_ + L"\r\n" + accept;
        if (!api.SendRequest(request.h, headers.c_str(), static_cast<DWORD>(headers.size()), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !api.ReceiveResponse(request.h, nullptr)) {
            error = "the server did not answer (" + WinError(GetLastError()) + ")";
            return false;
        }
        DWORD status = 0, size = sizeof(status);
        api.QueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                         WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            error = "the server answered HTTP " + std::to_string(status);
            return false;
        }
        std::uint64_t total = 0;
        wchar_t length[32]{};
        DWORD length_size = sizeof(length);
        if (api.QueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(length, nullptr, 10);
        return on_body(request.h, total, error);
    }

    std::wstring agent_;
};
}  // namespace

bool LoadSystemWinHttp(std::wstring* loaded_from, std::string* error) {
    const WinHttpApi& api = Api();
    if (loaded_from) *loaded_from = api.path;
    if (error) *error = api.error;
    return api.module != nullptr;
}

std::shared_ptr<HttpClient> MakeWinHttpClient(const std::string& user_agent) { return std::make_shared<WinHttpClient>(user_agent); }

// ---- Verifying a downloaded zip -------------------------------------------------------------
namespace {
constexpr char kZipExe[] = "StrandedTogether.exe";
constexpr char kZipDll[] = "winhttp.dll";
constexpr char kZipPackage[] = "stranded_together/package.json";
constexpr char kModPrefix[] = "stranded_together/";

// Files the launcher or the mod write in stranded_together\: never from a zip.
bool RuntimeFile(const std::string& lower_name) {
    static const char* const kRuntime[] = {"session.ini",         "session.last.ini", "link-status.json",       "session-record.json",
                                           "launcher-settings.json", "launcher.log",  "launcher.old.log",       "stranded_together.log",
                                           "update.zip"};
    const std::string leaf = lower_name.substr(sizeof(kModPrefix) - 1);
    for (const char* r : kRuntime)
        if (leaf == r) return true;
    return EndsWith(lower_name, ".st-new") || EndsWith(lower_name, ".st-old") || EndsWith(lower_name, ".old");
}

bool OpenZip(const std::wstring& path, ZipReader& reader, std::string& error) {
    ZipLimits limits;
    limits.max_entries = 64;
    limits.max_archive_size = kMaxUpdateZipBytes;
    limits.max_entry_size = 128ull << 20;
    limits.max_total_size = kMaxUpdateZipBytes;
    std::string zip_error;
    if (!reader.OpenFile(path, limits, zip_error)) {
        error = "it is not a readable zip (" + zip_error + ")";
        return false;
    }
    return true;
}

bool VerifyReader(const ZipReader& reader, const std::string& expected_version, ZipCheck& out, std::string& error) {
    out = ZipCheck{};
    SemVer expected;
    if (!ParseSemVer(expected_version, expected)) {
        error = "no version to compare with";
        return false;
    }
    for (const ZipEntry& e : reader.entries()) {
        const std::string lower = LowerAscii(e.name);
        if (e.is_directory) {
            if (lower != kModPrefix && !StartsWith(lower, kModPrefix)) {
                error = "it holds a folder outside stranded_together (" + e.name + ")";
                return false;
            }
            continue;
        }
        const bool top = lower == LowerAscii(kZipExe) || lower == kZipDll;
        if (!top && !StartsWith(lower, kModPrefix)) {
            error = "it holds a file the update never installs (" + e.name + ")";
            return false;
        }
        if (!top && RuntimeFile(lower)) {
            error = "it holds a file the launcher or the mod writes (" + e.name + ")";
            return false;
        }
        out.files.push_back(e.name);
    }
    const ZipEntry* exe = reader.Find(kZipExe);
    const ZipEntry* dll = reader.Find(kZipDll);
    const ZipEntry* pkg = reader.Find(kZipPackage);
    if (!exe || exe->is_directory || !dll || dll->is_directory || !pkg || pkg->is_directory) {
        error = "it lacks StrandedTogether.exe, winhttp.dll or stranded_together/package.json";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    std::string zip_error;
    json::Value v;
    if (!reader.Extract(*pkg, bytes, zip_error) || !json::Parse(std::string(bytes.begin(), bytes.end()), v) || !v.is_object() ||
        v.Str("product") != kPackageProduct || v.Int("v", 0) != 1) {
        error = "its package.json is not a Stranded Together package";
        return false;
    }
    SemVer got;
    out.version = v.Str("version");
    out.build_id = v.Str("build_id");
    if (!ParseSemVer(out.version, got) || CompareSemVer(got, expected) != 0) {
        error = "its package.json is version " + out.version + ", not " + expected_version;
        return false;
    }
    const json::Value* files = v.Find("files");
    if (!files || !files->is_array()) {
        error = "its package.json lists no files";
        return false;
    }
    std::vector<std::string> listed;
    for (const json::Value& row : files->Elements()) {
        const std::string path = row.is_object() ? row.Str("path") : std::string();
        const std::string sha = row.is_object() ? UpperAscii(row.Str("sha256")) : std::string();
        if (path.empty() || !IsHexText(sha, 64) || path == kZipPackage) {
            error = "its package.json has a damaged files[] row";
            return false;
        }
        if (std::find(listed.begin(), listed.end(), LowerAscii(path)) != listed.end()) {
            error = "its package.json lists " + path + " twice";
            return false;
        }
        listed.push_back(LowerAscii(path));
        const ZipEntry* e = reader.Find(path);
        if (!e || e->is_directory) {
            error = "package.json lists " + path + ", which the zip lacks";
            return false;
        }
        if (!reader.Extract(*e, bytes, zip_error)) {
            error = path + " is damaged (" + zip_error + ")";
            return false;
        }
        if (Sha256Hex(bytes.data(), bytes.size()) != sha) {
            error = path + " does not match its SHA-256 in package.json";
            return false;
        }
        if (const json::Value* size = row.Find("size"); size && size->is_number() && size->AsInt(-1) != static_cast<std::int64_t>(bytes.size())) {
            error = path + " does not have the size package.json gives";
            return false;
        }
        if (path == kZipDll && !LooksLikeOurDllBytes(std::string(bytes.begin(), bytes.end()))) {
            error = "its winhttp.dll is not the Stranded Together mod";
            return false;
        }
        if (path == kZipExe && (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z')) {
            error = "its StrandedTogether.exe is not a program";
            return false;
        }
    }
    for (const std::string& name : out.files) {
        if (name == kZipPackage) continue;
        if (std::find(listed.begin(), listed.end(), LowerAscii(name)) == listed.end()) {
            error = name + " is in the zip but not in its package.json";
            return false;
        }
    }
    if (std::find(listed.begin(), listed.end(), LowerAscii(kZipExe)) == listed.end() ||
        std::find(listed.begin(), listed.end(), LowerAscii(kZipDll)) == listed.end()) {
        error = "its package.json does not list StrandedTogether.exe and winhttp.dll";
        return false;
    }
    return true;
}
}  // namespace

bool VerifyUpdateZip(const std::wstring& zip_path, const std::string& expected_version, ZipCheck& out, std::string& error) {
    ZipReader reader;
    return OpenZip(zip_path, reader, error) && VerifyReader(reader, expected_version, out, error);
}

// ---- Installing it ----------------------------------------------------------------------------
namespace {
constexpr wchar_t kNewSuffix[] = L".st-new";
constexpr wchar_t kOldSuffix[] = L".st-old";

std::wstring TargetFor(const std::wstring& game, const std::string& zip_name) { return JoinPath2(game, Wide(zip_name)); }

bool WriteWholeFile(const std::wstring& path, const std::vector<std::uint8_t>& data, std::string& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = WinError(GetLastError());
        return false;
    }
    DWORD written = 0;
    const BOOL ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    const DWORD code = ok ? 0 : GetLastError();
    const BOOL flushed = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || (!data.empty() && written != data.size()) || !flushed) {
        error = code ? WinError(code) : std::string("the disk did not take all of it");
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

// A link (junction, symlink) between `top` (excluded) and `path` (included).
bool LinkBelow(const std::wstring& path, const std::wstring& top) {
    const std::wstring stop = FullPath(top);
    for (std::wstring cursor = FullPath(path); !cursor.empty() && !IEquals(cursor, stop) && PathInside(cursor, stop); cursor = ParentOf(cursor)) {
        if (IsReparsePoint(cursor)) return true;
        if (ParentOf(cursor) == cursor) break;
    }
    return false;
}

bool Transient(DWORD code) { return code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION; }
constexpr int kBusyRetries = 20;
constexpr DWORD kBusyRetryMs = 100;

bool MoveRetrying(const std::wstring& from, const std::wstring& to, DWORD* code_out = nullptr) {
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        const DWORD code = GetLastError();
        if (code_out) *code_out = code;
        if (!Transient(code) || attempt >= kBusyRetries) return false;
        Sleep(kBusyRetryMs);
    }
}

bool DeleteRetrying(const std::wstring& path, int retries = kBusyRetries) {
    for (int attempt = 0;; ++attempt) {
        if (!PathExists(path) || DeleteFileChecked(path)) return true;
        if (attempt >= retries) return false;
        Sleep(kBusyRetryMs);
    }
}

struct StagedFile {
    std::string name;   // the zip entry
    std::wstring target, staged, backup;
    bool moved_aside = false;   // the old target is at `backup`
    bool committed = false;     // the new file is at `target`
};
}  // namespace

const char* ApplyFailureName(ApplyFailure f) {
    switch (f) {
    case ApplyFailure::none: return "none";
    case ApplyFailure::damaged: return "damaged";
    case ApplyFailure::game_running: return "game_running";
    case ApplyFailure::foreign_dll: return "foreign_dll";
    case ApplyFailure::old_exe_in_use: return "old_exe_in_use";
    case ApplyFailure::install_failed: return "install_failed";
    case ApplyFailure::restart_failed: return "restart_failed";
    }
    return "?";
}

std::string ApplyFailureSentence(const ApplyReport& r) {
    switch (r.failure) {
    case ApplyFailure::none: return {};
    case ApplyFailure::damaged: return "The downloaded update is damaged or not a Stranded Together release. Nothing was changed. Try again later.";
    case ApplyFailure::game_running: return "Quit DEATH STRANDING first: the update replaces files the game uses. Nothing was changed.";
    case ApplyFailure::foreign_dll:
        return "Another program's winhttp.dll is in the game folder, so the update was not installed. Nothing was changed.";
    case ApplyFailure::old_exe_in_use:
        return "An earlier update is still finishing (StrandedTogether.exe.old is in use). Close other launcher windows, then try again.";
    case ApplyFailure::restart_failed:
        return r.restored ? "The updated launcher did not start, so your current version was put back. Try again, or download the zip from the release page."
                          : "The updated launcher did not start and some files could not be put back. Download the zip from the release page and extract it into the game folder again.";
    case ApplyFailure::install_failed: break;
    }
    return r.restored ? "The update could not be installed; your current version is back in place. Try again, or download the zip from the release page."
                      : "The update could not be installed and some files could not be put back. Download the zip from the release page and extract it into the game folder again.";
}

std::size_t CleanUpdateLeftovers(const InstancePaths& paths) {
    std::size_t handled = 0;
    for (const std::wstring& dir : {paths.game, paths.mod}) {
        if (!IsDir(dir) || LinkBelow(dir, paths.game)) continue;
        for (const DirEntry& e : ListDirectory(dir)) {
            if (e.directory) continue;
            const std::wstring path = JoinPath2(dir, e.name);
            const std::string lower = LowerAscii(Utf8(e.name));
            const bool is_new = EndsWith(lower, ".st-new"), is_old = EndsWith(lower, ".st-old");
            if (!is_new && !is_old) continue;
            const std::wstring target = path.substr(0, path.size() - 7);
            if (is_new || PathExists(target)) {
                if (DeleteFileChecked(path)) ++handled;
            } else if (MoveFileExW(path.c_str(), target.c_str(), 0)) {
                ++handled;   // the old file was moved aside and its new one never came: back it goes
            }
        }
    }
    return handled;
}

bool CleanOldExecutable(const InstancePaths& paths, int wait_ms) {
    const std::wstring old_exe = paths.launcher_exe + L".old";
    const int retries = wait_ms > 0 ? wait_ms / static_cast<int>(kBusyRetryMs) : 0;
    return DeleteRetrying(old_exe, retries);
}

bool ApplyUpdate(const InstancePaths& paths, const std::wstring& zip_path, const std::string& expected_version,
                 const ApplyOptions& options, ApplyReport& out, std::string& error) {
    out = ApplyReport{};
    auto failed = [&](ApplyFailure why, const std::string& detail) {
        out.failure = why;
        error = detail;
        return false;
    };
    if (options.game_running && options.game_running()) return failed(ApplyFailure::game_running, "ds.exe runs from the game folder");
    ZipReader reader;
    ZipCheck check;
    std::string why;
    if (!OpenZip(zip_path, reader, why) || !VerifyReader(reader, expected_version, check, why))
        return failed(ApplyFailure::damaged, "the update zip " + why);
    out.version = check.version;
    if (IsFile(paths.mod_dll) && !LooksLikeOurDll(paths.mod_dll)) return failed(ApplyFailure::foreign_dll, "winhttp.dll is not ours");
    // Switched off (only our winhttp.dll.disabled): the new DLL stays switched off.
    const bool dll_disabled = !PathExists(paths.mod_dll) && IsFile(paths.mod_dll_disabled) && LooksLikeOurDll(paths.mod_dll_disabled);

    // 1. Stage every new file next to its target. Nothing is replaced yet.
    std::vector<StagedFile> files;
    auto discard_staged = [&]() {
        for (const StagedFile& f : files)
            if (!f.committed) DeleteFileW(f.staged.c_str());
    };
    for (const ZipEntry& entry : reader.entries()) {
        std::wstring target = TargetFor(paths.game, entry.name);
        if (!entry.is_directory && entry.name == kZipDll && dll_disabled) target = paths.mod_dll_disabled;
        if (LinkBelow(target, paths.game)) {
            discard_staged();
            return failed(ApplyFailure::install_failed, entry.name + " in the game folder is a link");
        }
        if (entry.is_directory) {
            CreateDirectories(target);
            continue;
        }
        CreateDirectories(ParentOf(target));
        std::vector<std::uint8_t> data;
        std::string zip_error;
        if (!reader.Extract(entry, data, zip_error)) {
            discard_staged();
            return failed(ApplyFailure::damaged, entry.name + " is damaged (" + zip_error + ")");
        }
        StagedFile f;
        f.name = entry.name;
        f.target = target;
        f.staged = target + kNewSuffix;
        std::string write_error;
        if (!WriteWholeFile(f.staged, data, write_error)) {
            discard_staged();
            return failed(ApplyFailure::install_failed, "could not write " + entry.name + " (" + write_error + ")");
        }
        files.push_back(f);
    }

    // 2. Commit: move each old file aside and the new one in; undo all on any failure.
    const std::wstring old_exe = paths.launcher_exe + L".old";
    auto roll_back = [&]() {
        bool all = true;
        for (auto it = files.rbegin(); it != files.rend(); ++it) {
            if (it->moved_aside) {
                if (!MoveRetrying(it->backup, it->target)) all = false;   // replaces the new file
            } else if (it->committed && !DeleteRetrying(it->target)) {
                all = false;
            }
            it->committed = it->moved_aside = false;
        }
        discard_staged();
        return all;
    };
    if (!DeleteRetrying(old_exe)) {
        discard_staged();
        return failed(ApplyFailure::old_exe_in_use, "StrandedTogether.exe.old is in use");
    }
    if (options.game_running && options.game_running()) {
        discard_staged();
        return failed(ApplyFailure::game_running, "ds.exe started while the update was staged");
    }
    for (StagedFile& f : files) {
        f.backup = IEquals(f.target, paths.launcher_exe) ? old_exe : f.target + kOldSuffix;
        DWORD code = 0;
        if (options.before_commit && !options.before_commit(f.name)) {
            out.restored = roll_back();
            return failed(ApplyFailure::install_failed, "could not replace " + f.name + " (test)");
        }
        if (PathExists(f.target)) {
            if (f.backup != old_exe && PathExists(f.backup)) DeleteFileChecked(f.backup);
            if (!MoveRetrying(f.target, f.backup, &code)) {
                out.restored = roll_back();
                return failed(ApplyFailure::install_failed, "could not replace " + f.name + " (" + WinError(code) + ")");
            }
            f.moved_aside = true;
        }
        if (!MoveRetrying(f.staged, f.target, &code)) {
            out.restored = roll_back();
            return failed(ApplyFailure::install_failed, "could not put " + f.name + " in place (" + WinError(code) + ")");
        }
        f.committed = true;
    }
    out.files = files.size();

    // 3. The restart into the new launcher, while the old files can still come back.
    if (options.start_new) {
        std::string start_error;
        if (!options.start_new(start_error)) {
            out.restored = roll_back();
            return failed(ApplyFailure::restart_failed, start_error.empty() ? std::string("the new launcher did not start") : start_error);
        }
    }

    // 4. The old files are no longer needed; the running exe's .old goes when the new exe starts.
    for (const StagedFile& f : files)
        if (f.moved_aside && f.backup != old_exe) DeleteFileChecked(f.backup);
    DeleteFileChecked(zip_path);
    return true;
}

// ---- Restart after an update ------------------------------------------------------------------
std::wstring UpdateHandoffEventName(std::uint32_t pid) { return L"Local\\StrandedTogether-update-" + std::to_wstring(pid); }

std::wstring QuoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

bool StartSuccessor(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& dir, int ready_wait_ms,
                    std::string& error) {
    // Kept for the life of this process: the new exe opens it by name. Auto-reset.
    static const HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, UpdateHandoffEventName(GetCurrentProcessId()).c_str());
    if (!ready) {
        error = "Windows could not prepare the restart (" + WinError(GetLastError()) + ")";
        return false;
    }
    ResetEvent(ready);
    std::wstring command = QuoteArgument(exe);
    for (const std::wstring& a : args) command += L" " + QuoteArgument(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        error = "Windows could not start it (" + WinError(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    const HANDLE waits[2] = {ready, pi.hProcess};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, static_cast<DWORD>(ready_wait_ms < 0 ? 0 : ready_wait_ms));
    bool ok = r == WAIT_OBJECT_0;   // it runs and waits for this process to exit
    if (r == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        error = "it closed right after it started (exit code " + std::to_string(code) + ")";
    } else if (!ok) {
        // No answer in time (hung, or stuck in a crash dialog): end it so its exe can be
        // put back (a running image cannot be replaced) and the update rolls back.
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        error = r == WAIT_TIMEOUT ? "it did not answer within " + std::to_string(ready_wait_ms / 1000) + " seconds"
                                  : "Windows could not wait for it (" + WinError(GetLastError()) + ")";
    }
    CloseHandle(pi.hProcess);
    return ok;
}

TakeOver TakeOverFromPreviousLauncher(const PreviousLauncherOps& ops, int wait_ms) {
    if (!ops.signal || !ops.signal()) return TakeOver::none;
    if (ops.wait_exit && ops.wait_exit(wait_ms)) return TakeOver::exited;
    // The old launcher handed over but does not close: end it, it has nothing left to do.
    if (ops.stop) ops.stop();
    if (ops.wait_exit) ops.wait_exit(kStopPreviousWaitMs);
    return TakeOver::stopped;
}

PreviousLauncherOps PreviousLauncherWin32(std::uint32_t pid) {
    struct Handles {
        HANDLE event = nullptr, process = nullptr;
        ~Handles() {
            if (event) CloseHandle(event);
            if (process) CloseHandle(process);
        }
    };
    auto h = std::make_shared<Handles>();
    PreviousLauncherOps ops;
    ops.signal = [h, pid]() {
        if (!pid || pid == GetCurrentProcessId()) return false;
        // Only a launcher that is waiting for us created this event.
        h->event = OpenEventW(EVENT_MODIFY_STATE, FALSE, UpdateHandoffEventName(pid).c_str());
        if (!h->event) return false;
        // Opened before the signal, so the id cannot be reused while we wait. Without it we
        // could not wait for the old launcher to let go of the folder: do not claim the
        // handoff then (the old launcher sees this one exit and rolls the update back).
        h->process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (!h->process) return false;
        return SetEvent(h->event) != FALSE;
    };
    ops.wait_exit = [h](int ms) { return !h->process || WaitForSingleObject(h->process, static_cast<DWORD>(ms < 0 ? 0 : ms)) == WAIT_OBJECT_0; };
    ops.stop = [h]() { return h->process && TerminateProcess(h->process, 1) != FALSE; };
    return ops;
}

}  // namespace st::core
