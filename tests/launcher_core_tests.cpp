// Stranded Together launcher: pure-logic tests (no window, no game, no Steam).
// Join-code round trip, session.ini text, link-status parsing and its view,
// Steam library parsing and game-folder discovery, package.json and the
// winhttp.dll check (ours / foreign / disabled), the Debug page's options,
// redaction and the UI logic, plus a session activation/cleanup round trip
// and the real model in temporary fake game folders.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "core/core_install.hpp"
#include "core/core_model.hpp"
#include "core/core_paths.hpp"
#include "core/core_session.hpp"
#include "core/core_text.hpp"
#include "launcher_join_code.hpp"
#include "launcher_model_mock.hpp"
#include "launcher_ui_logic.hpp"

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

// A fake Env: no processes, no registry, fixed clock.
class FakeEnv final : public Env {
public:
    std::int64_t now = 1763208000000;   // 2025-11-15T12:00:00Z
    std::int64_t NowMs() override { return now; }
    void SleepMs(std::uint32_t ms) override { now += ms; }
    Civil LocalTime(std::int64_t utc_ms) override { return CivilFromMs(utc_ms); }
    bool RegDword(bool hklm, const std::wstring& key, const std::wstring& name, std::uint32_t& out) override {
        if (!steam || hklm || key != L"Software\\Valve\\Steam\\ActiveProcess") return false;
        if (name == L"pid") out = 555;
        else if (name == L"ActiveUser") out = 104729;
        else return false;
        return true;
    }
    bool RegString(bool hklm, const std::wstring& key, const std::wstring& name, std::wstring& out) override {
        if (steam_root.empty() || hklm || key != L"Software\\Valve\\Steam" || name != L"SteamPath") return false;
        out = steam_root;
        return true;
    }
    std::vector<ProcessRow> GameProcesses() override { return game_running ? std::vector<ProcessRow>{game} : std::vector<ProcessRow>{}; }
    bool ProcessStartMs(std::uint32_t, std::int64_t&) override { return false; }
    bool ProcessAlive(std::uint32_t pid) override {
        return (pid == 777 && owner_alive) || (pid == 555 && steam) || (pid == 9 && game_running);
    }
    std::uint32_t CurrentPid() override { return 4242; }
    std::int64_t CurrentStartMs() override { return 0; }
    bool OpenUrl(const std::wstring& url) override {
        last_url = url;
        return true;
    }
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
    bool owner_alive = false;
    bool steam = false;
    std::wstring steam_root;   // HKCU\Software\Valve\Steam\SteamPath
    std::wstring last_url;
    ProcessRow game;
};

std::wstring NewTempDir(const wchar_t* prefix) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + prefix + Wide(RandomHex(6));
    CreateDirectories(dir);
    return dir;
}

// Bytes that pass LooksLikeOurDll: a PE header, the proxy's GLOBALROOT forwarder
// and the mod folder's name in UTF-16 (what the real build carries).
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

std::string PackageJsonFor(const std::string& dll, const std::string& build_id, const std::string& exe = "") {
    std::string t = "{\"product\":\"stranded-together-package\",\"v\":1,\"version\":\"0.1.0\",\"build_id\":\"" + build_id +
                    "\",\"source_commit\":null,\"files\":[{\"path\":\"winhttp.dll\",\"sha256\":\"" + Sha256Hex(dll) + "\",\"size\":" +
                    std::to_string(dll.size()) + "}";
    if (!exe.empty()) t += ",{\"path\":\"StrandedTogether.exe\",\"sha256\":\"" + Sha256Hex(exe) + "\"}";
    return t + "]}";
}

void JoinCodes() {
    const std::string secret = "K7RD3WQ9MP2XVB6TZHN4";
    const std::uint64_t host = kSteamIdBase + 104729ull;
    const std::string code = FormatJoinCode(secret, host);
    CHECK(code.size() == 49);
    CHECK(code.rfind("DST1-", 0) == 0);
    CHECK(code.find("@" + Decimal(host)) != std::string::npos);
    std::string s;
    std::uint64_t h = 0;
    CHECK(ParseJoinCode(code, s, h) && s == secret && h == host);
    // Tolerant input: lower case, blanks, the invite prefix, O for 0.
    std::string loose = code;
    for (char& c : loose)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    CHECK(ParseJoinCode("  " + loose + "\r\n", s, h) && s == secret);
    CHECK(ParseJoinCode("+stranded_join " + code, s, h) && h == host);
    CHECK(CheckJoinCode(code, 0).ok);
    CHECK(!CheckJoinCode(code, host).ok);   // own code
    CHECK(CheckJoinCode("", 0).message.empty());
    CHECK(!CheckJoinCode("MGS2-AAAAA-AAAAA-AAAAA-AAAAA-AA@76561197960265729", 0).ok);
    std::string typo = code;
    typo[7] = typo[7] == 'Q' ? 'R' : 'Q';
    CHECK(!CheckJoinCode(typo, 0).ok);
    CHECK(!CheckJoinCode(code.substr(0, code.find('@')), 0).ok);
    CHECK(FormatJoinCode("short", host).empty());
    CHECK(IsCanonicalSecret(NewSecret()));
}

void SessionIniText() {
    SessionIni host;
    host.role = "host";
    host.secret = "K7RD3WQ9MP2XVB6TZHN4";
    std::string text, error;
    CHECK(MakeSessionIniText(host, text, error));
    CHECK(IsGeneratedIniText(text));
    CHECK(text.find("role=host\n") != std::string::npos);
    CHECK(text.find("transport=steam\n") != std::string::npos);
    CHECK(text.find("secret=K7RD3WQ9MP2XVB6TZHN4\n") != std::string::npos);
    CHECK(text.find("send_hz=30\n") != std::string::npos);
    CHECK(text.find("interp_delay_ms=100\n") != std::string::npos);
    CHECK(text.find("echo_delay_s=2\n") != std::string::npos);
    CHECK(text.find("echo_offset_m=1.5\n") != std::string::npos);
    CHECK(text.find("verbose=0\n") != std::string::npos);
    CHECK(text.find("partner_activate=1\n") != std::string::npos);
    CHECK(text.find("debug_menu=1\n") != std::string::npos);
    CHECK(text.find("debug_menu_key=F9\n") != std::string::npos);
    CHECK(text.find("one_shot=1\n") != std::string::npos);   // every launch is consumed by the mod
    CHECK(text.find("session.last.ini") != std::string::npos);
    CHECK(text.find("udp_") == std::string::npos);
    CHECK(text.find("peer_steamid") == std::string::npos);
    CHECK(text.find('\r') == std::string::npos);

    SessionIni guest = host;
    guest.role = "guest";
    CHECK(!MakeSessionIniText(guest, text, error));   // Steam guest needs the host's id
    guest.peer_steamid = "76561197960370457";
    guest.name = "Sam Porter";
    CHECK(MakeSessionIniText(guest, text, error));
    SessionIni back;
    CHECK(ParseSessionIniText(text, back, error));
    CHECK(back.role == "guest" && back.peer_steamid == guest.peer_steamid && back.secret == guest.secret && back.name == "Sam Porter");
    CHECK(back.echo_delay_s == 2.0 && back.echo_offset_m == 1.5 && back.send_hz == 30);
    CHECK(back.one_shot && back.debug_menu && back.debug_menu_key == 9 && back.partner_activate);

    // The Debug page's keys round-trip.
    SessionIni dbg = guest;
    dbg.debug_menu = false;
    dbg.debug_menu_key = 12;
    dbg.partner_activate = false;
    dbg.verbose = true;
    dbg.send_hz = 60;
    dbg.interp_delay_ms = 250;
    dbg.echo_delay_s = 3.5;
    dbg.echo_offset_m = 0.25;
    CHECK(MakeSessionIniText(dbg, text, error));
    CHECK(text.find("debug_menu=0\ndebug_menu_key=F12\n") != std::string::npos && text.find("partner_activate=0\n") != std::string::npos);
    CHECK(text.find("echo_delay_s=3.5\n") != std::string::npos && text.find("echo_offset_m=0.25\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.debug_menu && back.debug_menu_key == 12 && !back.partner_activate && back.verbose &&
          back.send_hz == 60 && back.interp_delay_ms == 250 && back.echo_delay_s == 3.5);
    dbg.debug_menu_key = 13;
    CHECK(!MakeSessionIniText(dbg, text, error));
    dbg.debug_menu_key = 9;
    dbg.echo_delay_s = 10.5;   // the mod's range is 0..10
    CHECK(!MakeSessionIniText(dbg, text, error));
    dbg.echo_delay_s = 2.0;
    dbg.one_shot = false;
    CHECK(MakeSessionIniText(dbg, text, error) && text.find("one_shot=0\n") != std::string::npos);
    CHECK(ParseFunctionKey("F9") == 9 && ParseFunctionKey("f12") == 12 && ParseFunctionKey("F13") == 0 && ParseFunctionKey("F0") == 0 &&
          ParseFunctionKey("F09") == 0 && ParseFunctionKey("0x78") == 0 && FunctionKeyName(1) == "F1" && FunctionKeyName(0).empty());

    // Debug tools only: role=solo, no secret.
    SessionIni solo;
    solo.role = "solo";
    solo.transport = "udp";
    solo.udp_peer = "127.0.0.1";
    solo.udp_port_local = 27555;
    solo.udp_port_peer = 27556;
    CHECK(MakeSessionIniText(solo, text, error) && text.find("role=solo\n") != std::string::npos && text.find("secret") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.role == "solo");
    solo.secret = "K7RD3WQ9MP2XVB6TZHN4";
    CHECK(!MakeSessionIniText(solo, text, error));

    SessionIni udp = guest;
    udp.transport = "udp";
    udp.udp_peer = "10.0.0.7";
    udp.udp_port_local = 27555;
    udp.udp_port_peer = 27556;
    CHECK(MakeSessionIniText(udp, text, error));
    CHECK(text.find("udp_peer=10.0.0.7\nudp_port_local=27555\nudp_port_peer=27556\n") != std::string::npos);
    udp.udp_peer = "10.0.0";
    CHECK(!MakeSessionIniText(udp, text, error));
    udp.udp_peer = "10.0.0.7";
    udp.udp_port_peer = 70000;
    CHECK(!MakeSessionIniText(udp, text, error));

    SessionIni echo;
    echo.role = "echo";
    echo.transport = "udp";
    echo.udp_peer = "127.0.0.1";
    echo.udp_port_local = 27555;
    echo.udp_port_peer = 27556;
    CHECK(MakeSessionIniText(echo, text, error));
    CHECK(text.find("secret=") == std::string::npos);

    SessionIni bad = host;
    bad.role = "spectator";
    CHECK(!MakeSessionIniText(bad, text, error));
    bad = host;
    bad.name = "a=b";
    CHECK(!MakeSessionIniText(bad, text, error));
    bad = host;
    bad.secret = "k7rd3wq9mp2xvb6tzhn4";   // not canonical
    CHECK(!MakeSessionIniText(bad, text, error));

    // A hand-written file (CRLF, BOM, comments) parses.
    CHECK(ParseSessionIniText("\xEF\xBB\xBF# test\r\nrole = echo\r\n; c\r\nverbose=1\r\n", back, error) && back.role == "echo" && back.verbose &&
          !back.one_shot);
    CHECK(!ParseSessionIniText("role=solo\ndebug_menu_key=F13\n", back, error));
    CHECK(!ParseSessionIniText("role=host\nsend_hz=0\n", back, error));
    CHECK(!IsGeneratedIniText("role=host\n"));
}

void StatusParse() {
    const std::string text =
        "{\"v\":1,\"pid\":1234,\"state\":\"connected\",\"transport\":\"udp\",\"route\":\"direct\",\"peer\":\"Fragile\",\"rtt_ms\":31,"
        "\"remote\":\"spawning\",\"reason\":\"\",\"code\":\"\",\"time_utc\":\"2025-11-15T12:00:00Z\"}";
    const json::Value s = ParseStatusText(text, 0);
    CHECK(s.is_object());
    CHECK(ParseStatusText(text, 1234).is_object());
    CHECK(ParseStatusText(text, 99).is_null());
    CHECK(ParseStatusText("{\"v\":1}", 0).is_null());
    CHECK(ParseStatusText("not json", 0).is_null());
    std::int64_t t = 0;
    ParseUtc("2025-11-15T12:00:14Z", t);
    CHECK(StatusTimeFresh(s, t));
    ParseUtc("2025-11-15T12:00:16Z", t);
    CHECK(!StatusTimeFresh(s, t));

    StatusView v = StatusViewOf(s, true, "guest");
    CHECK(v.connected && v.rtt_ms == 31 && v.tone == StatusView::Tone::good);
    CHECK(v.headline == "Connected to Fragile");
    CHECK(v.detail.find("appearing") != std::string::npos);
    v = StatusViewOf(s, false, "guest");
    CHECK(!v.connected && v.rtt_ms == 0 && v.headline == "The game is not responding");
    v = StatusViewOf(json::Value(), false, "host");
    CHECK(v.headline == "Waiting for the mod");

    for (const char* state : {"idle", "listening", "waiting", "connecting", "handshake", "reconnecting", "lost", "rejected", "error"}) {
        json::Value x = s;
        x["state"] = state;
        const StatusView sv = StatusViewOf(x, true, "host");
        CHECK(!sv.connected && !sv.headline.empty() && !sv.detail.empty());
    }
    json::Value lost = s;
    lost["state"] = "lost";
    lost["reason"] = "peer timed out";
    CHECK(StatusViewOf(lost, true, "host").detail.find("peer timed out") != std::string::npos);
    json::Value echo = s;
    echo["state"] = "idle";
    echo["remote"] = "visible";
    CHECK(StatusViewOf(echo, true, "echo").connected);
    // What the mod really writes: echo's own state, "off" before the link starts, steam:<id64> peers.
    echo["state"] = "echo";
    CHECK(StatusViewOf(echo, true, "echo").headline == "Solo test running");
    json::Value off = s;
    off["state"] = "off";
    off["reason"] = "";
    CHECK(StatusViewOf(off, true, "guest").headline == "Co-op is starting");
    off["reason"] = "no host SteamID in session.ini";
    const StatusView offv = StatusViewOf(off, true, "guest");
    CHECK(offv.tone == StatusView::Tone::bad && offv.detail.find("no host SteamID") != std::string::npos);
    CHECK(PeerDisplay("steam:76561197960370457") == "Steam player ...0457");
    CHECK(PeerDisplay("192.168.1.20:27555") == "192.168.1.20:27555");
    CHECK(PeerDisplay("") == "");
    json::Value steam_peer = s;
    steam_peer["peer"] = "steam:76561197960370457";
    const StatusView sp = StatusViewOf(steam_peer, true, "host");
    CHECK(sp.headline == "Connected to Steam player ...0457" && sp.headline.find("7656119") == std::string::npos);
}

void Discovery() {
    const std::string vdf =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
    const auto libs = SteamLibraryPathsFromVdf(vdf);
    CHECK(libs.size() == 2 && libs[1] == L"D:\\SteamLibrary");
    CHECK(InstallDirFromManifest("\"AppState\"\n{\n\t\"appid\"\t\t\"1850570\"\n\t\"installdir\"\t\t\"DS DC\"\n}\n") == "DS DC");
    CHECK(InstallDirFromManifest("") == "DEATH STRANDING DIRECTORS CUT");
    const InstancePaths p = GetInstancePaths(L"C:\\Games\\DS");
    CHECK(p.exe == L"C:\\Games\\DS\\ds.exe");
    CHECK(p.mod_dll == L"C:\\Games\\DS\\winhttp.dll");
    CHECK(p.session_ini == L"C:\\Games\\DS\\stranded_together\\session.ini");
    CHECK(p.link_status == L"C:\\Games\\DS\\stranded_together\\link-status.json");
    CHECK(p.mod_log == L"C:\\Games\\DS\\stranded_together\\stranded_together.log");
    CHECK(p.package_json == L"C:\\Games\\DS\\stranded_together\\package.json");
    CHECK(p.session_last_ini == L"C:\\Games\\DS\\stranded_together\\session.last.ini");
    CHECK(p.mod_dll_disabled == L"C:\\Games\\DS\\winhttp.dll.disabled");
    CHECK(p.crash_report == L"C:\\Games\\DS\\error.txt");
    CHECK(p.launcher_exe == L"C:\\Games\\DS\\StrandedTogether.exe");
    CHECK(PlayMutexName(L"C:\\Games\\DS") == PlayMutexName(L"c:\\games\\ds\\"));
    CHECK(!GamePathProblem(L"C:\\").empty());

    // Steam's game folder from HKCU SteamPath -> libraryfolders.vdf -> appmanifest installdir.
    const std::wstring root = NewTempDir(L"st-launcher-steam-");
    const std::wstring steam = JoinPath2(root, L"Steam");
    const std::wstring lib = JoinPath2(root, L"Library2");
    const std::wstring game = JoinPath2(lib, L"steamapps\\common\\DS DC");
    std::string error;
    CHECK(CreateDirectories(JoinPath2(steam, L"steamapps")) && CreateDirectories(game));
    std::string escaped;
    for (char c : Utf8(lib)) {
        escaped.push_back(c);
        if (c == '\\') escaped.push_back('\\');
    }
    CHECK(WriteAtomic(JoinPath2(steam, L"steamapps\\libraryfolders.vdf"),
                      "\"libraryfolders\"\n{\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + escaped + "\"\n\t}\n}\n", true, error));
    CHECK(WriteAtomic(JoinPath2(lib, L"steamapps\\appmanifest_1850570.acf"), std::string("\"AppState\"\n{\n\t\"installdir\"\t\t\"DS DC\"\n}\n"),
                      true, error));
    FakeEnv env;
    CHECK(FindGameFolders(env).empty());
    env.steam_root = steam;
    CHECK(FindGameFolders(env).empty());   // no ds.exe yet
    CHECK(WriteAtomic(JoinPath2(game, L"ds.exe"), std::string(), true, error));
    const auto found = FindGameFolders(env);
    CHECK(found.size() == 1 && IEquals(found[0], game));
    {
        // The launcher started from a download folder: the wrong-folder page offers Steam's copy.
        const std::wstring downloads = JoinPath2(root, L"Downloads");
        CHECK(CreateDirectories(downloads));
        auto m = MakeRealModelWithEnv(downloads, std::make_shared<FakeEnv>(env));
        CHECK(m->phase() == Phase::not_in_game_folder && IEquals(Wide(m->discovered_game_dir()), game));
    }
    RemoveTree(root);
}

// package.json and what winhttp.dll is.
void PackageAndMod() {
    const std::string dll = FakeOurDll("build-a");
    PackageInfo pkg = ParsePackageText(PackageJsonFor(dll, "0.1.0+abc1234"));
    CHECK(pkg.present && pkg.ok && pkg.version == "0.1.0" && pkg.build_id == "0.1.0+abc1234" && pkg.dll_sha256 == Sha256Hex(dll) &&
          pkg.source_commit.empty());
    CHECK(!ParsePackageText("{}").ok && !ParsePackageText("not json").ok);
    CHECK(!ParsePackageText("{\"product\":\"stranded-together-package\",\"v\":1,\"version\":\"0.1.0\",\"files\":[]}").ok);
    CHECK(!ParsePackageText("{\"product\":\"other\",\"v\":1,\"version\":\"0.1.0\",\"files\":[{\"path\":\"winhttp.dll\",\"sha256\":\"" +
                            Sha256Hex(dll) + "\"}]}").ok);
    CHECK(LooksLikeOurDllBytes(dll));
    CHECK(!LooksLikeOurDllBytes(std::string()) && !LooksLikeOurDllBytes("MZ" + std::string(100, 'x')));
    CHECK(!LooksLikeOurDllBytes("MZ" + std::string(62, '\0') + "\\\\.\\GLOBALROOT\\SystemRoot\\System32\\winhttp.dll.X"));

    const std::wstring dir = NewTempDir(L"st-launcher-pkg-");
    std::string error;
    CHECK(WriteAtomic(JoinPath2(dir, L"ds.exe"), std::string(), true, error));
    const InstancePaths paths = GetInstancePaths(dir);
    FakeEnv env;
    PackageInfo none = ReadPackage(paths);
    CHECK(!none.present && !none.ok);
    CHECK(CheckMod(paths, none, "0.1.0").state == ModState::missing);

    // Somebody else's winhttp.dll: foreign, never moved.
    CHECK(WriteAtomic(paths.mod_dll, std::string("MZ another loader"), true, error));
    ModCheck m = CheckMod(paths, none, "0.1.0");
    CHECK(m.state == ModState::foreign && !m.note.empty());
    CHECK(!SetModEnabled(env, paths, none, false, error) && IsFile(paths.mod_dll) && !IsFile(paths.mod_dll_disabled));
    DeleteFileChecked(paths.mod_dll);

    // Ours without package.json: a development build.
    CHECK(WriteAtomic(paths.mod_dll, dll, true, error));
    CHECK(CheckMod(paths, none, "0.1.0").state == ModState::dev_build);

    // With the package: ok; another build id, another dll or another exe: mismatch.
    CHECK(CreateDirectories(paths.mod));
    CHECK(WriteAtomic(paths.package_json, PackageJsonFor(dll, "0.1.0+abc1234", "exe-a"), true, error));
    pkg = ReadPackage(paths);
    CHECK(pkg.ok);
    CHECK(CheckMod(paths, pkg, "0.1.0+abc1234").state == ModState::ok);
    CHECK(CheckMod(paths, pkg, "0.1.0+fff0000").state == ModState::mismatch);
    CHECK(WriteAtomic(paths.launcher_exe, std::string("exe-b"), true, error));
    CHECK(CheckMod(paths, pkg, "0.1.0+abc1234").state == ModState::mismatch);
    CHECK(WriteAtomic(paths.launcher_exe, std::string("exe-a"), true, error));
    CHECK(CheckMod(paths, pkg, "0.1.0+abc1234").state == ModState::ok);
    CHECK(WriteAtomic(paths.mod_dll, FakeOurDll("build-b"), true, error));
    m = CheckMod(paths, pkg, "0.1.0+abc1234");
    CHECK(m.state == ModState::mismatch && m.note.find("0.1.0") != std::string::npos);
    CHECK(WriteAtomic(paths.package_json, std::string("{broken"), true, error));
    pkg = ReadPackage(paths);
    CHECK(pkg.present && !pkg.ok && CheckMod(paths, pkg, "0.1.0").state == ModState::mismatch);
    CHECK(WriteAtomic(paths.package_json, PackageJsonFor(dll, "0.1.0+abc1234"), true, error));
    pkg = ReadPackage(paths);
    CHECK(WriteAtomic(paths.mod_dll, dll, true, error));

    // Disable / enable: only ours, never over another file, never while the game runs.
    env.game_running = true;
    env.game.pid = 9;
    env.game.path = paths.exe;
    CHECK(!SetModEnabled(env, paths, pkg, false, error) && IsFile(paths.mod_dll));
    env.game_running = false;
    CHECK(SetModEnabled(env, paths, pkg, false, error) && !IsFile(paths.mod_dll) && IsFile(paths.mod_dll_disabled));
    CHECK(CheckMod(paths, pkg, "0.1.0+abc1234").state == ModState::disabled);
    CHECK(!SetModEnabled(env, paths, pkg, false, error));   // nothing to switch off
    CHECK(WriteAtomic(paths.mod_dll, std::string("MZ another loader"), true, error));
    CHECK(!SetModEnabled(env, paths, pkg, true, error) && IsFile(paths.mod_dll_disabled));   // never overwrites
    DeleteFileChecked(paths.mod_dll);
    CHECK(SetModEnabled(env, paths, pkg, true, error) && IsFile(paths.mod_dll) && !IsFile(paths.mod_dll_disabled));
    CHECK(CheckMod(paths, pkg, "0.1.0+abc1234").state == ModState::ok);
    RemoveTree(dir);
}

void DebugOptionsPersist() {
    DebugOptions o;
    o.echo_delay_s = 99.0;   // out of range -> default
    o.send_hz = 0;
    o.debug_menu_key = 13;
    o.echo_offset_m = 3.333;
    const DebugOptions c = ClampDebugOptions(o);
    CHECK(c.echo_delay_s == 2.0 && c.send_hz == 30 && c.debug_menu_key == 9 && c.echo_offset_m == 3.33);
    DebugOptions d;
    d.echo_delay_s = 4.5;
    d.send_hz = 60;
    d.interp_delay_ms = 200;
    d.partner_activate = false;
    d.verbose = true;
    d.debug_menu = false;
    d.debug_menu_key = 5;
    json::Value settings = json::Value::Object();
    DebugOptionsToSettings(d, settings);
    CHECK(settings.Find("debug") && settings.Find("debug")->Str("debug_menu_key") == "F5");
    json::Value back;
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions r = DebugOptionsFromSettings(back);
    CHECK(r.echo_delay_s == 4.5 && r.send_hz == 60 && r.interp_delay_ms == 200 && !r.partner_activate && r.verbose && !r.debug_menu &&
          r.debug_menu_key == 5);
    SessionIni ini;
    ApplyDebugOptions(r, ini);
    CHECK(ini.one_shot && ini.echo_delay_s == 4.5 && ini.debug_menu_key == 5 && !ini.debug_menu && ini.verbose);
    CHECK(DebugOptionsFromSettings(json::Value::Object()).debug_menu_key == 9);
    // Hand-edited numbers far outside int range fall back instead of an undefined cast.
    json::Value wild;
    CHECK(json::Parse("{\"debug\":{\"send_hz\":1e300,\"interp_delay_ms\":-1e300,\"echo_delay_s\":\"x\"}}", wild));
    const DebugOptions w = DebugOptionsFromSettings(wild);
    CHECK(w.send_hz == 30 && w.interp_delay_ms == 100 && w.echo_delay_s == 2.0);
}

void Redaction() {
    const std::string code = FormatJoinCode("K7RD3WQ9MP2XVB6TZHN4", kSteamIdBase + 104729ull);
    const std::string r = Redact("join " + code + " secret=K7RD3WQ9MP2XVB6TZHN4 udp_peer=81.2.3.4 +stranded_join " + code);
    CHECK(r.find("K7RD3") == std::string::npos);
    CHECK(r.find("DST1-<redacted>") != std::string::npos);
    CHECK(r.find("81.2.x.x") != std::string::npos);
    CHECK(Redact("udp_peer=192.168.1.20").find("192.168.1.20") != std::string::npos);   // private addresses stay
    CHECK(RedactForLog("C:\\Users\\sam\\x.log").find("sam") == std::string::npos);
}

void UiLogic() {
    CHECK(ui::PageAfterPhaseChange(Phase::ready, Phase::launching, ui::Page::play) == ui::Page::in_game);
    CHECK(ui::PageAfterPhaseChange(Phase::in_game, Phase::ready, ui::Page::in_game) == ui::Page::play);
    CHECK(ui::PageAfterPhaseChange(Phase::ready, Phase::launching, ui::Page::tools) == ui::Page::tools);
    CHECK(!ui::PageEnabled(Phase::ready, ui::Page::in_game));
    InstallChecks c;
    c.steam_running = false;
    CHECK(!ui::CanHost(Phase::ready, c));
    c.steam_running = true;
    CHECK(ui::CanHost(Phase::ready, c));
    CHECK(!ui::CanHost(Phase::in_game, c));
    CHECK(ui::EvaluateJoinField("  ", CodeCheck{}, false).can_join == false);
    CHECK(ui::EvaluateJoinField("x", CodeCheck{true, "ok"}, false).can_join);
    CHECK(!ui::EvaluateJoinField("x", CodeCheck{true, "ok"}, true).can_join);
    const ui::CodeDisplay d = ui::SplitJoinCode("DST1-A@7656");
    CHECK(d.main == "DST1-A" && d.account == "@7656");
    int port = 0;
    CHECK(ui::ParsePort("27555", port) && port == 27555);
    CHECK(!ui::ParsePort("0", port) && !ui::ParsePort("65536", port) && !ui::ParsePort("12a", port));
    LanSettings lan;
    CHECK(!ui::LanProblem(lan).empty());
    lan.peer_ip = "192.168.1.20";
    CHECK(ui::LanProblem(lan).empty());
    lan.peer_ip = "192.168.1.256";
    CHECK(!ui::LanProblem(lan).empty());
    CHECK(ui::RouteText("udp", "") == "LAN (direct)" && ui::RouteText("steam", "relay") == "Steam relay");
    const ui::Options o = ui::ParseCommandLine({L"--write-session", L"join", L"--code", L"DST1-X", L"--transport", L"udp", L"--udp-port-local", L"27555"});
    CHECK(o.error.empty() && o.mode == ui::Options::Mode::write_session && o.write_port_local == 27555);
    CHECK(!ui::ParseCommandLine({L"--write-session", L"join"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--code", L"x"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--selftest", L"--watch", L"abc"}).error.empty());
    CHECK(ui::PageEnabled(Phase::ready, ui::Page::debug) && ui::PageEnabled(Phase::error, ui::Page::debug) &&
          ui::PageEnabled(Phase::in_game, ui::Page::debug) && !ui::PageEnabled(Phase::not_in_game_folder, ui::Page::debug));
    CHECK(ui::PageAfterPhaseChange(Phase::ready, Phase::launching, ui::Page::debug) == ui::Page::debug);
    CHECK(ui::TabForRole(Role::solo) == ui::PlayTab::solo);
    CHECK(std::string(ui::PageLabel(ui::Page::debug)) == "Debug");
    InstallChecks mc;
    mc.mod_state = ModState::ok;
    mc.package_version = "0.1.0";
    CHECK(ui::ModChipText(mc) == "Mod 0.1.0" && ui::ModTone(ModState::ok) == ui::Tone::good);
    mc.mod_state = ModState::dev_build;
    CHECK(ui::ModChipText(mc) == "Mod: development build");
    mc.mod_state = ModState::foreign;
    CHECK(ui::ModTone(ModState::foreign) == ui::Tone::bad && ui::ModChipText(mc).find("not ours") != std::string::npos);
    CHECK(ui::ModTone(ModState::mismatch) == ui::Tone::warn && ui::ModTone(ModState::disabled) == ui::Tone::warn);
    CHECK(ui::LastLines("a\nb\nc\n", 2) == "b\nc" && ui::LastLines("a\r\nb", 5) == "a\nb" && ui::LastLines("", 3).empty() &&
          ui::LastLines("x", 0).empty());
    {
        const char* items = ui::FunctionKeyItems();
        int n = 0;
        for (const char* q = items; *q; q += std::strlen(q) + 1) ++n;
        CHECK(n == 12);
    }
    const ui::Options so = ui::ParseCommandLine({L"--write-session", L"solo"});
    CHECK(so.error.empty() && so.write_role == L"solo");
    CHECK(ui::SceneCount() == static_cast<int>(ui::Scene::count) && ui::SceneCount() == 35);
    for (int i = 0; i < ui::SceneCount(); ++i) CHECK(*ui::SceneName(static_cast<ui::Scene>(i)) != '\0');
}

void MockFlow() {
    MockModel m(MockOptions{});
    m.use_manual_clock(0.0);
    std::string code, error;
    CHECK(m.host(code, error));
    m.advance(15.0);
    m.tick();
    CHECK(m.phase() == Phase::in_game && m.link().connected && m.link().remote == "visible");
    m.advance(200.0);
    m.tick();
    CHECK(m.phase() == Phase::ready);
    CHECK(m.debug_solo(error) && m.session_role() == Role::solo && m.session_transport() == Transport::udp);
    m.advance(15.0);
    m.tick();
    CHECK(m.link().connected && m.link().headline == "Debug tools running");
    m.advance(200.0);
    m.tick();
    CHECK(m.set_mod_enabled(false, error) && m.phase() == Phase::error && m.checks().mod_state == ModState::disabled);
    m.recheck();
    CHECK(m.phase() == Phase::error);
    CHECK(m.set_mod_enabled(true, error) && m.phase() == Phase::ready && m.checks().mod_state == ModState::ok);
    CHECK(!m.mod_log_tail(4096).empty() && !m.has_crash_report());
}

// Activation and cleanup in a temporary fake game folder (a zero-byte ds.exe).
void SessionRoundTrip() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"st-launcher-test-" + Wide(RandomHex(6));
    CHECK(CreateDirectories(dir));
    std::string error;
    CHECK(WriteAtomic(JoinPath2(dir, L"ds.exe"), std::string(), true, error));
    const InstancePaths paths = GetInstancePaths(dir);
    FakeEnv env;
    SessionIni ini;
    ini.role = "host";
    ini.secret = "K7RD3WQ9MP2XVB6TZHN4";
    std::string id;
    CHECK(ActivateSession(env, paths, ini, id, error));
    CHECK(id.size() == 32);
    std::string text;
    CHECK(ReadTextShared(paths.session_ini, text) && text.find("role=host") != std::string::npos);
    json::Value record;
    CHECK(ReadSessionRecord(paths, record) && RecordIsActive(record) && record.Str("ini_sha256") == Sha256Hex(text));

    // The game runs: no cleanup, no second session.
    env.game_running = true;
    env.game.pid = 9;
    env.game.path = paths.exe;
    CHECK(!CleanupSession(env, paths).ran);
    CHECK(IsFile(paths.session_ini));
    CHECK(!ActivateSession(env, paths, ini, id, error));

    // The game exited: the session is stale (its window, pid 4242, is this process: never counted).
    env.game_running = false;
    CHECK(StaleSessionPending(env, paths));
    const CleanupResult c = CleanupSession(env, paths);
    CHECK(c.ran && c.ini_removed && !IsFile(paths.session_ini));
    CHECK(ReadSessionRecord(paths, record) && !RecordIsActive(record));

    // A session.ini that is not ours is never deleted.
    CHECK(WriteAtomic(paths.session_ini, std::string("role=echo\n"), true, error));
    const CleanupResult foreign = CleanupSession(env, paths);
    CHECK(foreign.ran && foreign.ini_kept && IsFile(paths.session_ini));
    CHECK(!ActivateSession(env, paths, ini, id, error));
    DeleteFileChecked(paths.session_ini);

    // Settings and the gate on the fake folder.
    json::Value settings;
    ReadSettings(paths, settings);
    const GateResult gate = CheckGate(env, paths, settings);
    CHECK(gate.game_found && gate.exe_hashed && !gate.exe_known && !gate.mod_present);
    CHECK(!UntestedVersionText(gate).empty());
    CHECK(settings.Find("exe_cache") != nullptr);
    CHECK(SaveSettings(paths, settings, error));
    RemoveTree(dir);
}

// The real model on a temporary fake game folder (zero-byte ds.exe and winhttp.dll) with a fake Env.
void RealModelFlow() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring dir = std::wstring(temp) + L"st-launcher-model-" + Wide(RandomHex(6));
    CHECK(CreateDirectories(dir));
    std::string error;
    CHECK(WriteAtomic(JoinPath2(dir, L"ds.exe"), std::string(), true, error));
    const InstancePaths paths = GetInstancePaths(dir);
    auto env = std::make_shared<FakeEnv>();
    {
        auto m = MakeRealModelWithEnv(dir, env);
        CHECK(m->phase() == Phase::error && !m->checks().mod_present && !m->last_error().empty());
    }
    CHECK(WriteAtomic(paths.mod_dll, std::string("MZ another loader"), true, error));
    {
        auto foreign = MakeRealModelWithEnv(dir, env);
        CHECK(foreign->phase() == Phase::error && foreign->checks().mod_state == ModState::foreign && !foreign->checks().mod_present);
        CHECK(!foreign->set_mod_enabled(false, error) && IsFile(paths.mod_dll));   // never touched
    }
    CHECK(WriteAtomic(paths.mod_dll, FakeOurDll(), true, error));
    auto m = MakeRealModelWithEnv(dir, env);
    CHECK(m->phase() == Phase::ready);
    CHECK(m->checks().mod_present && m->checks().mod_state == ModState::dev_build && !m->checks().version_tested && !m->checks().steam_running);
    std::string code;
    CHECK(!m->host(code, error) && error.find("Steam") != std::string::npos);
    env->steam = true;
    m->tick();
    CHECK(m->checks().steam_running);
    CHECK(m->host(code, error));
    CHECK(m->phase() == Phase::launching && env->last_url == L"steam://rungameid/1850570");
    CHECK(m->join_code() == code && code.find("@" + Decimal(kSteamIdBase + 104729ull)) != std::string::npos);
    std::string text;
    SessionIni ini;
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error));
    CHECK(ini.role == "host" && ini.transport == "steam" && FormatJoinCode(ini.secret, kSteamIdBase + 104729ull) == code);
    CHECK(ini.one_shot && ini.debug_menu && ini.debug_menu_key == 9);
    CHECK(!m->check_code(code).ok);   // own code

    // The game starts; the mod consumes the one-shot session.ini (renames it) and reports in.
    env->game_running = true;
    env->game.pid = 9;
    env->game.path = paths.exe;
    m->tick();
    CHECK(m->phase() == Phase::in_game);
    CHECK(MoveFileExW(paths.session_ini.c_str(), paths.session_last_ini.c_str(), MOVEFILE_REPLACE_EXISTING));
    {
        // A window opened while the game runs finds the host's code in session.last.ini.
        auto reopened = MakeRealModelWithEnv(dir, env);
        CHECK(reopened->phase() == Phase::in_game && reopened->session_role() == Role::host && reopened->join_code() == code);
    }
    const std::string status = "{\"v\":1,\"pid\":9,\"state\":\"connected\",\"transport\":\"steam\",\"route\":\"relay\",\"peer\":\"Fragile\","
                               "\"rtt_ms\":40,\"remote\":\"visible\",\"reason\":\"\",\"code\":\"\",\"time_utc\":\"" +
                               FormatUtcSeconds(env->now) + "\"}";
    CHECK(WriteAtomic(paths.link_status, status, true, error));
    env->now += 600;
    m->tick();
    CHECK(m->link().connected && m->link().ping_ms == 40 && m->link().peer == "Fragile");
    env->now += 20000;   // the status goes stale
    m->tick();
    CHECK(!m->link().connected && m->link().headline == "The game is not responding");
    CHECK(!m->end_session(error));   // not while the game runs

    // The game exits: session.ini goes, the launcher is ready again.
    env->game_running = false;
    m->tick();
    env->now += 5000;
    m->tick();
    CHECK(m->phase() == Phase::after_game && !IsFile(paths.session_ini) && !IsFile(paths.link_status) && IsFile(paths.session_last_ini));
    env->now += 5000;
    m->tick();
    CHECK(m->phase() == Phase::ready);

    // Join, cancel; solo test; LAN mode.
    CHECK(!m->join("DST1-nonsense", error));
    const std::string partner = FormatJoinCode("7QK2MX9RT4PB8WZ3HCNA", kSteamIdBase + 7919ull);
    CHECK(m->join("+stranded_join " + partner, error) && m->session_role() == Role::join);
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "guest" &&
          ini.peer_steamid == Decimal(kSteamIdBase + 7919ull) && ini.secret == "7QK2MX9RT4PB8WZ3HCNA");
    CHECK(m->cancel_launch(error) && m->phase() == Phase::ready && !IsFile(paths.session_ini));
    CHECK(m->solo_test(error) && m->session_role() == Role::echo);
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "echo" &&
          ini.transport == "udp" && ini.udp_peer == "127.0.0.1" && ini.secret.empty());
    CHECK(m->cancel_launch(error));

    // The Debug page: options go into session.ini; debug tools only = role solo.
    DebugOptions dbg;
    dbg.echo_delay_s = 3.5;
    dbg.echo_offset_m = 4.0;
    dbg.send_hz = 60;
    dbg.interp_delay_ms = 150;
    dbg.partner_activate = false;
    dbg.verbose = true;
    dbg.debug_menu_key = 6;
    dbg.debug_menu = false;   // debug tools only switches the menu on anyway
    m->set_debug_options(dbg);
    CHECK(m->debug_solo(error) && m->session_role() == Role::solo && m->phase() == Phase::launching);
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "solo" && ini.transport == "udp" &&
          ini.udp_peer == "127.0.0.1" && ini.secret.empty() && ini.one_shot && ini.echo_delay_s == 3.5 && ini.echo_offset_m == 4.0 &&
          ini.send_hz == 60 && ini.interp_delay_ms == 150 && !ini.partner_activate && ini.verbose && ini.debug_menu_key == 6 &&
          ini.debug_menu);
    CHECK(text.find("debug_menu_key=F6\n") != std::string::npos);
    CHECK(!m->set_mod_enabled(false, error));   // not during a session
    CHECK(m->clear_session(error) && m->phase() == Phase::ready && !IsFile(paths.session_ini));
    CHECK(m->solo_test(error));
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "echo" && ini.echo_delay_s == 3.5);
    CHECK(m->cancel_launch(error));

    // Clear session also removes a hand-written session.ini (End session leaves it).
    CHECK(WriteAtomic(paths.session_ini, std::string("role=echo\n"), true, error));
    m->tick();
    CHECK(!m->end_session(error) && IsFile(paths.session_ini));
    CHECK(m->clear_session(error) && !IsFile(paths.session_ini));

    // The live log and the crash report.
    CHECK(m->mod_log_tail(4096).empty() && !m->has_crash_report());
    CHECK(WriteAtomic(paths.mod_log, std::string("line 1\nline 2\n"), true, error));
    CHECK(m->mod_log_tail(4096).find("line 2") != std::string::npos);
    CHECK(WriteAtomic(paths.crash_report, std::string("crash"), true, error));
    env->now += 3000;
    m->tick();
    CHECK(m->has_crash_report() && m->crash_report(text) && text == "crash");

    // Disable / enable the mod from the Debug page.
    CHECK(m->set_mod_enabled(false, error) && m->phase() == Phase::error && m->checks().mod_state == ModState::disabled);
    CHECK(!m->debug_solo(error));
    CHECK(m->set_mod_enabled(true, error) && m->phase() == Phase::ready && m->checks().mod_state == ModState::dev_build);

    // A release install: package.json names this winhttp.dll and this launcher build.
    CHECK(WriteAtomic(paths.package_json, PackageJsonFor(FakeOurDll(), m->version()), true, error));
    env->now += 3000;
    m->tick();
    CHECK(m->checks().mod_state == ModState::ok && m->checks().package_version == "0.1.0");
    CHECK(WriteAtomic(paths.mod_dll, FakeOurDll("other"), true, error));
    env->now += 3000;
    m->tick();
    CHECK(m->phase() == Phase::ready && m->checks().mod_state == ModState::mismatch && !m->checks().mod_note.empty());
    CHECK(WriteAtomic(paths.mod_dll, std::string("MZ another loader"), true, error));
    env->now += 3000;
    m->tick();
    CHECK(m->phase() == Phase::error && m->checks().mod_state == ModState::foreign);
    CHECK(WriteAtomic(paths.mod_dll, FakeOurDll(), true, error));
    env->now += 3000;
    m->tick();
    CHECK(m->phase() == Phase::ready && m->checks().mod_state == ModState::ok);
    m->set_debug_options(DebugOptions{});
    m->set_transport(Transport::udp);
    CHECK(!m->join(partner, error) && error.find("LAN") != std::string::npos);
    LanSettings lan;
    lan.peer_ip = "192.168.1.20";
    lan.port_local = 27555;
    lan.port_peer = 27556;
    m->set_lan(lan);
    m->set_player_name("Sam");
    CHECK(m->join(partner, error) && m->session_transport() == Transport::udp);
    CHECK(ReadTextShared(paths.session_ini, text) && text.find("udp_peer=192.168.1.20\n") != std::string::npos &&
          text.find("name=Sam\n") != std::string::npos);
    CHECK(m->cancel_launch(error));

    // Choices persist; a leftover session.ini is cleaned on the next start.
    {
        auto again = MakeRealModelWithEnv(dir, env);
        CHECK(again->transport() == Transport::udp && again->lan().peer_ip == "192.168.1.20" && again->player_name() == "Sam");
        CHECK(again->debug_options().debug_menu_key == 9 && again->debug_options().send_hz == 30);
    }
    SessionIni host;
    host.role = "host";
    host.secret = "K7RD3WQ9MP2XVB6TZHN4";
    std::string id;
    CHECK(ActivateSession(*env, paths, host, id, error) && IsFile(paths.session_ini));
    {
        auto again = MakeRealModelWithEnv(dir, env);   // the record's window is this process: never counted as live
        CHECK(again->phase() == Phase::ready && !IsFile(paths.session_ini));
    }
    RemoveTree(dir);
}
}  // namespace

int main() {
    JoinCodes();
    SessionIniText();
    StatusParse();
    Discovery();
    PackageAndMod();
    DebugOptionsPersist();
    Redaction();
    UiLogic();
    MockFlow();
    SessionRoundTrip();
    RealModelFlow();
    std::printf("launcher core tests: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
