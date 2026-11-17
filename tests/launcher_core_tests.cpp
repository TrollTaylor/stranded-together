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
#include <set>
#include <string>

#include "core/core_install.hpp"
#include "core/core_model.hpp"
#include "core/core_paths.hpp"
#include "core/core_sandbox.hpp"
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
        return (pid == 777 && owner_alive) || (pid == 555 && steam) || (pid == 9 && game_running) || alive.count(pid) != 0;
    }
    // The local two-instance test starts ds.exe directly: recorded, pid 31, 32, ...
    bool StartProcess(const std::wstring& exe, const std::wstring& cwd, const std::vector<std::wstring>& env_add,
                      std::uint32_t& pid_out) override {
        started.push_back(env_add);
        started_exe = exe;
        (void)cwd;
        pid_out = 30 + static_cast<std::uint32_t>(started.size());
        alive.insert(pid_out);
        return true;
    }
    std::vector<std::vector<std::wstring>> started;
    std::wstring started_exe;
    std::set<std::uint32_t> alive;
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

// The character choice (native/mod/body_table.cpp's offered bodies) and the partner body in session.ini.
void Characters() {
    // The list is the table's offered bodies, in id order, Sam first (ids 1-16 are proven in game).
    const std::vector<CharacterChoice> list = OfferedCharacters();
    CHECK(list.size() == 16);
    if (list.size() == 16) {
        CHECK(list[0].key == "sam" && list[1].key == "porter" && list[2].key == "fragile" && list[3].key == "diehardman" &&
              list[4].key == "cliff" && list[6].key == "deadman" && list[15].key == "fragile_suit");
        CHECK(list[0].label == "Sam (porter suit)" && list[2].label == "Fragile" && list[4].label == "Cliff Unger" &&
              list[7].label == "Heartman");
    }
    CHECK(IsOfferedCharacter("fragile") && IsOfferedCharacter("deadman") && !IsOfferedCharacter("Fragile") &&
          !IsOfferedCharacter("gandalf") && !IsOfferedCharacter(""));
    CHECK(CharacterKeyOrSam("Fragile") == "fragile" && CharacterKeyOrSam("cliff") == "cliff" && CharacterKeyOrSam("Higgs") == "higgs");
    CHECK(CharacterKeyOrSam("gandalf") == "sam");   // not in the table
    CHECK(CharacterKeyOrSam("nobody") == "sam" && CharacterKeyOrSam("") == "sam");
    CHECK(CharacterLabel("diehardman") == "Die-Hardman" && CharacterLabel("nobody") == "Sam (porter suit)");

    std::string text, error;
    SessionIni back;
    // The host is always Sam: written even when nothing (or another character) was asked for.
    SessionIni host;
    host.role = "host";
    host.secret = "K7RD3WQ9MP2XVB6TZHN4";
    CHECK(MakeSessionIniText(host, text, error) && text.find("my_character=sam\n") != std::string::npos);
    CHECK(text.find("partner_body") == std::string::npos);   // not set: the mod's default
    host.my_character = "fragile";
    host.partner_body = "real";
    CHECK(MakeSessionIniText(host, text, error) && text.find("my_character=sam\n") != std::string::npos &&
          text.find("fragile") == std::string::npos && text.find("partner_body=real\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.my_character == "sam" && back.partner_body == "real");
    // The guest's key goes out as chosen.
    SessionIni guest = host;
    guest.role = "guest";
    guest.peer_steamid = "76561197960370457";
    guest.partner_body = "holo";
    CHECK(MakeSessionIniText(guest, text, error) && text.find("my_character=fragile\n") != std::string::npos &&
          text.find("partner_body=holo\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.my_character == "fragile" && back.partner_body == "holo");
    guest.my_character.clear();   // not chosen: not written (the mod sends sam)
    CHECK(MakeSessionIniText(guest, text, error) && text.find("my_character") == std::string::npos);
    // Only offered keys (exact spelling); partner_body only real or holo.
    guest.my_character = "gandalf";
    CHECK(!MakeSessionIniText(guest, text, error) && error.find("fragile") != std::string::npos);
    guest.my_character = "Fragile";
    CHECK(!MakeSessionIniText(guest, text, error));
    guest.my_character = "cliff";
    guest.partner_body = "marker";
    CHECK(!MakeSessionIniText(guest, text, error));
    guest.partner_body = "real";
    CHECK(MakeSessionIniText(guest, text, error) && text.find("my_character=cliff\n") != std::string::npos);
    // Echo and solo carry the choice too (harmless without a partner).
    SessionIni echo;
    echo.role = "echo";
    echo.transport = "udp";
    echo.udp_peer = "127.0.0.1";
    echo.udp_port_local = 27555;
    echo.udp_port_peer = 27556;
    echo.my_character = "porter";
    CHECK(MakeSessionIniText(echo, text, error) && text.find("my_character=porter\n") != std::string::npos);
    // Self view: written only for a non-host whose character is set and not sam; parsed back.
    CHECK(text.find("self_character") == std::string::npos);   // off unless asked
    echo.self_character = true;
    CHECK(MakeSessionIniText(echo, text, error) && text.find("self_character=1\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.self_character && back.my_character == "porter");
    guest.self_character = true;
    guest.my_character = "fragile";
    CHECK(MakeSessionIniText(guest, text, error) && text.find("self_character=1\n") != std::string::npos);
    guest.my_character = "sam";   // Sam needs no self view
    CHECK(MakeSessionIniText(guest, text, error) && text.find("self_character") == std::string::npos);
    guest.my_character.clear();
    CHECK(MakeSessionIniText(guest, text, error) && text.find("self_character") == std::string::npos);
    host.self_character = true;   // the host is always Sam
    CHECK(MakeSessionIniText(host, text, error) && text.find("self_character") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.self_character);
    // Name tags: the mod's default is on, so only name_tags=0 is written; parsed back either way.
    CHECK(text.find("name_tags") == std::string::npos && back.name_tags);
    host.name_tags = false;
    CHECK(MakeSessionIniText(host, text, error) && text.find("name_tags=0\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.name_tags);
    CHECK(ParseSessionIniText(text + "name_tags=1\n", back, error) && back.name_tags);
    // Player collision: the same pattern (default on, only partner_collision=0 written).
    CHECK(text.find("partner_collision") == std::string::npos && back.partner_collision);
    host.partner_collision = false;
    CHECK(MakeSessionIniText(host, text, error) && text.find("partner_collision=0\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.partner_collision);
    CHECK(ParseSessionIniText(text + "partner_collision=1\n", back, error) && back.partner_collision);
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
    // Partner body: real by default (also for older settings files without the key), holo when chosen.
    CHECK(DebugOptions{}.real_partner_body && DebugOptionsFromSettings(json::Value::Object()).real_partner_body);
    CHECK(r.real_partner_body && settings.Find("debug")->Str("partner_body") == "real" && ini.partner_body == "real");
    d.real_partner_body = false;
    DebugOptionsToSettings(d, settings);
    CHECK(settings.Find("debug")->Str("partner_body") == "holo");
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions h = DebugOptionsFromSettings(back);
    CHECK(!h.real_partner_body);
    ApplyDebugOptions(h, ini);
    CHECK(ini.partner_body == "holo");
    // Self view: on by default (older settings files too), saved, and only with the real body.
    CHECK(DebugOptions{}.self_character && DebugOptionsFromSettings(json::Value::Object()).self_character);
    CHECK(h.self_character && !ini.self_character);   // Hologram: no pose hook, no self view
    ApplyDebugOptions(r, ini);
    CHECK(ini.self_character && ini.partner_body == "real");
    d.real_partner_body = true;
    d.self_character = false;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions so = DebugOptionsFromSettings(back);
    CHECK(!so.self_character && so.real_partner_body);
    ApplyDebugOptions(so, ini);
    CHECK(!ini.self_character);
    // Name tags: on by default (older settings files too), saved, copied into the session.ini.
    CHECK(DebugOptions{}.name_tags && DebugOptionsFromSettings(json::Value::Object()).name_tags && ini.name_tags);
    d.name_tags = false;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions nt = DebugOptionsFromSettings(back);
    CHECK(!nt.name_tags);
    ApplyDebugOptions(nt, ini);
    CHECK(!ini.name_tags);
    // Player collision: on by default (older settings files too), saved, copied into the session.ini.
    CHECK(DebugOptions{}.partner_collision && DebugOptionsFromSettings(json::Value::Object()).partner_collision && ini.partner_collision);
    d.partner_collision = false;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions pc = DebugOptionsFromSettings(back);
    CHECK(!pc.partner_collision);
    ApplyDebugOptions(pc, ini);
    CHECK(!ini.partner_collision);
    json::Value odd;
    CHECK(json::Parse("{\"debug\":{\"partner_body\":\"marker\"}}", odd) && DebugOptionsFromSettings(odd).real_partner_body);
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
    CHECK(ui::SceneCount() == static_cast<int>(ui::Scene::count) && ui::SceneCount() == 39);
    CHECK(std::string(ui::SceneName(ui::Scene::debug_test_saves)) == "36-debug-test-saves" &&
          std::string(ui::SceneName(ui::Scene::debug_local_test)) == "37-debug-local-test" &&
          std::string(ui::SceneName(ui::Scene::play_join_characters)) == "38-play-join-characters" &&
          std::string(ui::SceneName(ui::Scene::play_join_hologram)) == "39-play-join-hologram");
    CHECK(std::string(ui::DebugTabLabel(ui::DebugTab::saves)) == "Test saves" && ui::kDebugTabCount == 3);
    for (int i = 0; i < ui::SceneCount(); ++i) CHECK(*ui::SceneName(static_cast<ui::Scene>(i)) != '\0');
}

void MockFlow() {
    MockModel m(MockOptions{});
    m.use_manual_clock(0.0);
    CHECK(m.my_character() == "sam" && m.local_test().guest_character == "sam");
    m.set_my_character("Fragile");
    m.set_local_test_guest_character("gandalf");   // not a character
    CHECK(m.my_character() == "fragile" && m.local_test().guest_character == "sam");
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
    CHECK(ini.my_character == "sam" && ini.partner_body == "real" && m->my_character() == "sam");
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
    m->set_my_character("diehardman");
    CHECK(m->my_character() == "diehardman");
    CHECK(m->join("+stranded_join " + partner, error) && m->session_role() == Role::join);
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "guest" &&
          ini.peer_steamid == Decimal(kSteamIdBase + 7919ull) && ini.secret == "7QK2MX9RT4PB8WZ3HCNA");
    CHECK(ini.my_character == "diehardman" && ini.partner_body == "real");
    CHECK(ini.self_character && text.find("self_character=1\n") != std::string::npos);   // the default: see yourself as your character
    CHECK(m->cancel_launch(error) && m->phase() == Phase::ready && !IsFile(paths.session_ini));
    m->set_my_character("gandalf");   // not in the mod's table: Sam
    CHECK(m->my_character() == "sam");
    m->set_my_character("fragile");
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
    dbg.real_partner_body = false;
    m->set_debug_options(dbg);
    CHECK(m->debug_solo(error) && m->session_role() == Role::solo && m->phase() == Phase::launching);
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "solo" && ini.transport == "udp" &&
          ini.udp_peer == "127.0.0.1" && ini.secret.empty() && ini.one_shot && ini.echo_delay_s == 3.5 && ini.echo_offset_m == 4.0 &&
          ini.send_hz == 60 && ini.interp_delay_ms == 150 && !ini.partner_activate && ini.verbose && ini.debug_menu_key == 6 &&
          ini.debug_menu);
    CHECK(text.find("debug_menu_key=F6\n") != std::string::npos && text.find("partner_body=holo\n") != std::string::npos);
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
        CHECK(again->debug_options().debug_menu_key == 9 && again->debug_options().send_hz == 30 && again->debug_options().real_partner_body);
        CHECK(again->my_character() == "fragile");
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

void WriteFile(const std::wstring& path, const std::string& text) {
    std::string error;
    CreateDirectories(ParentOf(path));
    WriteAtomic(path, text, true, error);
}

// Test saves: the seed-folder check, seeding a sandbox (the real save tree is only read), the
// session.ini keys, the real model's Play with test save and the local two-instance test.
void TestSaves() {
    const std::wstring local = NewTempDir(L"st-launcher-local-");
    wchar_t old_local[1024]{};
    const DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", old_local, 1024);
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());   // RealSaveRoot() -> this fake tree
    const std::wstring real = JoinPath2(local, L"KojimaProductions");
    CHECK(IEquals(RealSaveRoot(), real));
    const std::wstring acct = JoinPath2(JoinPath2(real, L"DeathStrandingDC"), L"104729");
    WriteFile(JoinPath2(acct, L"profile\\profile.dat"), "own profile");
    WriteFile(JoinPath2(acct, L"profile\\game_settings.cfg"), "\"GameDifficulty\" \"4\"\r\n");
    WriteFile(JoinPath2(acct, L"autosave0\\checkpoint.dat"), "own slot");
    WriteFile(JoinPath2(acct, L"steam_autocloud.vdf"), "steam");
    CreateDirectories(JoinPath2(real, L"DeathStranding\\104729\\transfer"));
    const std::string real_before = SaveTreeManifest(real);
    CHECK(real_before.find("profile\\profile.dat") != std::string::npos);

    const std::wstring downloads = NewTempDir(L"st-launcher-dl-");
    const std::wstring save100 = JoinPath2(downloads, L"complete-100");
    WriteFile(JoinPath2(save100, L"DeathStrandingDC\\autosave3\\checkpoint.dat"), "100 percent");
    WriteFile(JoinPath2(save100, L"DeathStrandingDC\\manualsave1\\checkpoint.dat"), "100 percent manual");
    WriteFile(JoinPath2(save100, L"DeathStrandingDC\\notaslot\\checkpoint.dat"), "x");
    SeedCheck c = CheckSeedFolder(save100);
    CHECK(c.ok && c.slots == 2 && c.note.find("2 save slots") != std::string::npos);
    CHECK(CheckSeedFolder(JoinPath2(save100, L"DeathStrandingDC")).ok);
    CHECK(!CheckSeedFolder(downloads).ok && !CheckSeedFolder(downloads).note.empty());
    CHECK(!CheckSeedFolder(L"").ok && !CheckSeedFolder(JoinPath2(downloads, L"missing")).ok);
    const SeedCheck own = CheckSeedFolder(real);   // <root>\DeathStrandingDC\<account>\<slots>
    CHECK(own.ok && own.slots == 1);

    // A fake game folder.
    const std::wstring dir = NewTempDir(L"st-launcher-sandbox-");
    std::string error;
    CHECK(WriteAtomic(JoinPath2(dir, L"ds.exe"), std::string(), true, error));
    CHECK(WriteAtomic(JoinPath2(dir, L"winhttp.dll"), FakeOurDll(), true, error));
    const InstancePaths paths = GetInstancePaths(dir);
    const SandboxPaths sp = GetSandboxPaths(paths, kTestSaveSandbox);
    CHECK(PathInside(sp.root, JoinPath2(paths.mod, L"sandbox")) && !SandboxSeeded(sp));
    CHECK(SeedSandbox(paths, sp, &c, real, 104729, "complete-100", error));
    CHECK(SandboxSeeded(sp));
    const std::wstring sacct = JoinPath2(sp.saves, L"DeathStrandingDC\\104729");
    std::string text;
    CHECK(ReadTextShared(JoinPath2(sacct, L"profile\\profile.dat"), text) && text == "own profile");   // your profile
    CHECK(ReadTextShared(JoinPath2(sacct, L"autosave3\\checkpoint.dat"), text) && text == "100 percent");   // the test save's slots
    CHECK(IsFile(JoinPath2(sacct, L"manualsave1\\checkpoint.dat")) && !IsDir(JoinPath2(sacct, L"autosave0")) && !IsDir(JoinPath2(sacct, L"notaslot")));
    CHECK(!IsFile(JoinPath2(sacct, L"steam_autocloud.vdf")) && IsDir(JoinPath2(sp.saves, L"DeathStranding\\104729\\transfer")));
    // Without a seed: a full copy of your own saves.
    const SandboxPaths full = GetSandboxPaths(paths, kLocalHostSandbox);
    CHECK(SeedSandbox(paths, full, nullptr, real, 0, "own", error) && IsFile(JoinPath2(full.saves, L"DeathStrandingDC\\104729\\autosave0\\checkpoint.dat")));
    // Never outside stranded_together\sandbox, never overlapping the real tree.
    SandboxPaths bad = sp;
    bad.root = JoinPath2(dir, L"elsewhere");
    bad.saves = JoinPath2(bad.root, L"saves");
    CHECK(!SeedSandbox(paths, bad, &c, real, 104729, "x", error) && !IsDir(bad.root));
    SandboxPaths inside_real = sp;
    inside_real.root = JoinPath2(paths.mod, L"sandbox\\x");
    CHECK(!SeedSandbox(paths, inside_real, &c, inside_real.root, 104729, "x", error));
    SeedCheck empty;
    CHECK(!SeedSandbox(paths, sp, &empty, real, 104729, "x", error));
    SeedCheck in_sandbox = c;   // a test save picked inside the test copies: refused before anything is emptied
    in_sandbox.slots_dir = JoinPath2(sp.saves, L"DeathStrandingDC\\104729");
    CHECK(!SeedSandbox(paths, sp, &in_sandbox, real, 104729, "x", error) && error.find("outside") != std::string::npos && SandboxSeeded(sp));
    CHECK(SaveTreeManifest(real) == real_before);   // the real tree was only read
    CHECK(ManifestChanges(real_before, real_before).empty());
    CHECK(ManifestChanges("a\t1\tX\nb\t1\tY\n", "a\t1\tZ\nc\t1\tW\n").size() == 3);
    CHECK(ResetSandbox(paths, full, error) && !IsDir(full.root));
    CHECK(!ResetSandbox(paths, bad, error));

    // session.ini: the sandbox keys round trip; invalid combinations are refused.
    SessionIni ini;
    ini.role = "solo";
    ini.transport = "udp";
    ini.udp_peer = "127.0.0.1";
    ini.udp_port_local = 27555;
    ini.udp_port_peer = 27556;
    ini.sandbox_saves = "local-guest";
    ini.sandbox_foreign_saves = true;
    ini.appdir_settings = true;
    ini.second_instance = true;
    ini.local_input = "guest";
    ini.frame_cap = 30;
    CHECK(MakeSessionIniText(ini, text, error));
    CHECK(text.find("sandbox_saves=local-guest\n") != std::string::npos && text.find("second_instance=1\n") != std::string::npos &&
          text.find("local_input=guest\n") != std::string::npos && text.find("frame_cap=30\n") != std::string::npos);
    SessionIni back;
    CHECK(ParseSessionIniText(text, back, error) && back.sandbox_saves == "local-guest" && back.sandbox_foreign_saves && back.appdir_settings &&
          back.second_instance && back.local_input == "guest" && back.frame_cap == 30);
    SessionIni bad_ini = ini;
    bad_ini.sandbox_saves = "..\\x";
    CHECK(!MakeSessionIniText(bad_ini, text, error));
    bad_ini = ini;
    bad_ini.sandbox_saves.clear();   // second_instance without a sandbox
    CHECK(!MakeSessionIniText(bad_ini, text, error));
    bad_ini = ini;
    bad_ini.local_input = "both";
    CHECK(!MakeSessionIniText(bad_ini, text, error));
    bad_ini = ini;
    bad_ini.frame_cap = 5;
    CHECK(!MakeSessionIniText(bad_ini, text, error));
    SessionIni plain;
    plain.role = "solo";
    CHECK(MakeSessionIniText(plain, text, error) && text.find("sandbox") == std::string::npos && text.find("frame_cap") == std::string::npos);

    // The real model: pick the folder, Play with test save.
    CHECK(ResetSandbox(paths, sp, error));
    auto env = std::make_shared<FakeEnv>();
    env->steam = true;
    {
        auto m = MakeRealModelWithEnv(dir, env);
        CHECK(m->phase() == Phase::ready);
        CHECK(!m->test_save().folder_ok && !m->play_test_save(error) && error.find("test save") != std::string::npos);
        CHECK(!m->set_test_save_folder(Utf8(downloads), error) && !error.empty());
        CHECK(m->set_test_save_folder(Utf8(save100), error) && m->test_save().folder_ok);
        CHECK(m->play_test_save(error));
        CHECK(m->phase() == Phase::launching && m->session_role() == Role::solo && SandboxSeeded(sp));
        // One of the launcher's own copies is refused (changing the folder would delete it), copy untouched.
        CHECK(!m->set_test_save_folder(Utf8(sp.saves), error) && error.find("test copies") != std::string::npos);
        CHECK(m->test_save().folder == Utf8(save100) && SandboxSeeded(sp));
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error));
        CHECK(ini.role == "solo" && ini.sandbox_saves == "test" && ini.sandbox_foreign_saves && !ini.second_instance && ini.debug_menu);
        // The game runs and exits; the real tree is compared.
        env->game_running = true;
        env->game.pid = 9;
        env->game.path = paths.exe;
        m->tick();
        CHECK(m->phase() == Phase::in_game);
        env->game_running = false;
        m->tick();
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::after_game && m->test_save().tone == TestSaveView::Tone::good &&
              m->test_save().result.find("not changed") != std::string::npos);
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::ready);
        // Remembered; "use for all" puts the test save into a Solo test too.
        m->set_use_test_save(true);
    }
    {
        auto m = MakeRealModelWithEnv(dir, env);
        CHECK(m->test_save().folder == Utf8(save100) && m->test_save().use_for_all && m->test_save().seeded);
        CHECK(m->solo_test(error));
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "echo" && ini.sandbox_saves == "test");
        CHECK(m->end_session(error));
        // A refusal by the mod (no redirect) is shown and ends the session.
        m->set_use_test_save(false);
        CHECK(m->play_test_save(error));
        WriteFile(JoinPath2(paths.mod, L"sandbox-refused.txt"), "sandbox=REFUSED\nreason=self-test failed\n");
        m->tick();
        CHECK(m->phase() == Phase::ready && m->test_save().tone == TestSaveView::Tone::bad &&
              m->test_save().result.find("self-test failed") != std::string::npos);
        DeleteFileChecked(JoinPath2(paths.mod, L"sandbox-refused.txt"));

        // The local two-instance test.
        m->set_local_test_host_pad(true);
        m->set_local_test_guest_character("cliff");
        CHECK(m->local_test().guest_character == "cliff");
        CHECK(m->start_local_test(error));
        CHECK(env->started.size() == 1 && IEquals(env->started_exe, paths.exe));
        const auto has = [&](std::size_t i, const std::wstring& v) {
            for (const std::wstring& e : env->started[i]) if (e == v) return true;
            return false;
        };
        CHECK(has(0, L"SteamAppId=1850570") && has(0, L"ST_INSTANCE_DIR=instances\\host"));
        const std::wstring host_ini = JoinPath2(paths.mod, L"instances\\host\\session.ini");
        const std::wstring guest_ini = JoinPath2(paths.mod, L"instances\\guest\\session.ini");
        CHECK(ReadTextShared(host_ini, text) && ParseSessionIniText(text, ini, error));
        CHECK(ini.role == "host" && ini.transport == "udp" && ini.udp_port_local == 27555 && ini.udp_port_peer == 27556 &&
              ini.sandbox_saves == "local-host" && ini.appdir_settings && !ini.second_instance && ini.local_input == "host_pad" &&
              ini.frame_cap == 30 && ini.debug_menu && ini.local_window == "left");
        CHECK(ini.my_character == "sam" && ini.partner_body == "real" && !ini.self_character);   // the host game is always Sam
        const std::string secret = ini.secret;
        CHECK(ReadTextShared(guest_ini, text) && ParseSessionIniText(text, ini, error));
        CHECK(ini.role == "guest" && ini.udp_port_local == 27556 && ini.udp_port_peer == 27555 && ini.sandbox_saves == "local-guest" &&
              ini.second_instance && ini.local_input == "guest" && ini.secret == secret && ini.local_window == "right");
        CHECK(ini.my_character == "cliff" && ini.partner_body == "real" && ini.self_character);   // the guest sees itself as Cliff
        CHECK(SandboxSeeded(GetSandboxPaths(paths, kLocalHostSandbox)) && SandboxSeeded(GetSandboxPaths(paths, kLocalGuestSandbox)));
        CHECK(IsFile(GetSandboxPaths(paths, kLocalGuestSandbox).settings));
        CHECK(m->local_test().running && m->local_test().host_pid == 31 && m->local_test().guest_pid == 0);
        CHECK(!m->start_local_test(error) && !m->reset_test_saves(error));
        CHECK(!m->play_test_save(error) && error.find("local two-player test") != std::string::npos);   // one test at a time
        env->now += 9000;
        m->tick();
        CHECK(env->started.size() == 2 && has(1, L"ST_INSTANCE_DIR=instances\\guest") && m->local_test().guest_pid == 32);
        env->alive.clear();   // both games quit
        m->tick();
        env->now += 4000;
        m->tick();
        CHECK(!m->local_test().running && m->local_test().tone == LinkView::Tone::good && !IsFile(host_ini) && !IsFile(guest_ini));
        {
            auto reopened = MakeRealModelWithEnv(dir, env);   // the guest's character is remembered
            CHECK(reopened->local_test().guest_character == "cliff");
        }

        // A local game's refusal left behind never speaks for a later "Play with test save" game.
        const std::wstring active = JoinPath2(paths.mod, L"sandbox-active.txt");
        WriteFile(JoinPath2(paths.mod, L"instances\\host\\sandbox-refused.txt"), "sandbox=REFUSED\nreason=old local refusal\n");
        CHECK(m->play_test_save(error));
        env->game_running = true;
        m->tick();
        CHECK(m->phase() == Phase::in_game);
        WriteFile(active, "sandbox=redirected\n");   // the mod's DllMain marker
        env->now += 25000;
        m->tick();
        CHECK(m->test_save().tone != TestSaveView::Tone::bad);
        env->game_running = false;
        m->tick();
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::after_game && m->test_save().tone == TestSaveView::Tone::good &&
              m->test_save().result.find("old local refusal") == std::string::npos);
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::ready);
        // A game that never confirms the test save (its session.ini did not reach it): warned while it runs and after.
        CHECK(m->play_test_save(error) && !IsFile(active));   // the old marker is gone before the launch
        env->game_running = true;
        m->tick();
        CHECK(m->phase() == Phase::in_game);
        env->now += 25000;
        m->tick();
        CHECK(m->test_save().tone == TestSaveView::Tone::bad && m->test_save().result.find("WARNING") != std::string::npos);
        env->game_running = false;
        m->tick();
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::after_game && m->test_save().tone == TestSaveView::Tone::bad &&
              m->test_save().result.find("never confirmed") != std::string::npos);
        env->now += 5000;
        m->tick();
        CHECK(m->reset_test_saves(error) && !SandboxSeeded(sp));
    }
    CHECK(SaveTreeManifest(real) == real_before);   // after everything: the real tree untouched

    SetEnvironmentVariableW(L"LOCALAPPDATA", had ? old_local : nullptr);
    RemoveTree(dir);
    RemoveTree(downloads);
    RemoveTree(local);
}
}  // namespace

// The local test forces windowed, 1280x720 into the settings.cfg the game rewrote (fullscreen):
// without window_mode=1 the host went fullscreen and the guest could not start (v0.5.2).
void LocalTestSettings() {
    const std::string fresh = LocalTestSettingsCfg("");
    CHECK(fresh.find("\"window_mode\" \"1\"") != std::string::npos);
    CHECK(fresh.find("\"rendering_width\" \"1280\"") != std::string::npos);
    const std::string game = "\"rendering_width\" \"1920\"\r\n\"window_mode\" \"0\"\r\n\"nvidia_dlss\" \"0\"\r\n";
    const std::string merged = LocalTestSettingsCfg(game);
    CHECK(merged.find("\"window_mode\" \"1\"") != std::string::npos);
    CHECK(merged.find("\"window_mode\" \"0\"") == std::string::npos);
    CHECK(merged.find("\"rendering_width\" \"1280\"") != std::string::npos);
    CHECK(merged.find("1920") == std::string::npos);
    CHECK(merged.find("\"nvidia_dlss\" \"0\"") != std::string::npos);  // the game's other keys stay
}

int main() {
    JoinCodes();
    SessionIniText();
    Characters();
    StatusParse();
    Discovery();
    PackageAndMod();
    DebugOptionsPersist();
    Redaction();
    UiLogic();
    MockFlow();
    SessionRoundTrip();
    RealModelFlow();
    TestSaves();
    LocalTestSettings();
    std::printf("launcher core tests: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
