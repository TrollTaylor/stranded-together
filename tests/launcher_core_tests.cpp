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

#include <chrono>
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
#include "core/core_textures.hpp"
#include "extract_plan.hpp"
#include "tex_hash.hpp"
#include "tex_manifest.hpp"
#include "tex_slots.hpp"
#include "launcher_join_code.hpp"
#include "launcher_model_mock.hpp"
#include "launcher_ui_logic.hpp"
#include "mod/sandbox.hpp"   // the mod's side of the coop-guest folder name

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

// A directory junction at `link` (created) to `target` (FSCTL_SET_REPARSE_POINT; no admin rights needed). Removed with
// RemoveDirectoryW(link), which never touches the target.
bool MakeJunction(const std::wstring& link, const std::wstring& target) {
    wchar_t full[MAX_PATH * 2] = {};
    if (!GetFullPathNameW(target.c_str(), MAX_PATH * 2, full, nullptr)) return false;
    const std::wstring sub = std::wstring(L"\\??\\") + full;
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE h = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    // REPARSE_DATA_BUFFER, MountPointReparseBuffer: tag, data length, reserved, then four USHORT offsets / lengths and
    // the substitute name and an empty print name, each NUL terminated.
    const USHORT sub_bytes = static_cast<USHORT>(sub.size() * sizeof(wchar_t));
    const USHORT data_len = static_cast<USHORT>(8 + sub_bytes + sizeof(wchar_t) + sizeof(wchar_t));
    std::string buf(8 + data_len, '\0');
    unsigned char* p = reinterpret_cast<unsigned char*>(&buf[0]);
    const ULONG tag = IO_REPARSE_TAG_MOUNT_POINT;
    const USHORT fields[4] = {0, sub_bytes, static_cast<USHORT>(sub_bytes + sizeof(wchar_t)), 0};
    std::memcpy(p, &tag, 4);
    std::memcpy(p + 4, &data_len, 2);
    std::memcpy(p + 8, fields, sizeof(fields));
    std::memcpy(p + 16, sub.c_str(), sub_bytes);
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, 0x000900A4 /* FSCTL_SET_REPARSE_POINT */, p, static_cast<DWORD>(buf.size()), nullptr, 0, &got, nullptr);
    CloseHandle(h);
    if (!ok) RemoveDirectoryW(link.c_str());
    return ok != 0;
}

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
    // The guest plays Fragile (porter suit); others only with the Debug option "Allow other characters".
    CHECK(std::string(kGuestCharacter) == "fragile_suit" && IsOfferedCharacter(kGuestCharacter) &&
          CharacterLabel(kGuestCharacter) == "Fragile (porter suit)");
    CHECK(GuestPickOrDefault("Cliff") == "cliff" && GuestPickOrDefault("sam") == "sam" && GuestPickOrDefault("gandalf") == "fragile_suit" &&
          GuestPickOrDefault("") == "fragile_suit");
    CHECK(GuestCharacter("cliff", false) == "fragile_suit" && GuestCharacter("sam", false) == "fragile_suit" &&
          GuestCharacter("cliff", true) == "cliff" && GuestCharacter("sam", true) == "sam" && GuestCharacter("gandalf", true) == "fragile_suit");
    {
        const std::vector<CharacterChoice> only = GuestCharacterChoices(false), all = GuestCharacterChoices(true);
        CHECK(only.size() == 1 && only[0].key == "fragile_suit" && only[0].label == "Fragile (porter suit)");
        CHECK(all.size() == 16 && all[0].key == "sam");
    }
    {
        // Saved choices from before (no "character_defaults"): the old default "sam" (or nothing) becomes
        // Fragile (porter suit), a real pick is kept; with the marker, "sam" is a pick like any other.
        json::Value old = json::Value::Object();
        CHECK(GuestPickFromSettings(old, "sam") == "fragile_suit" && GuestPickFromSettings(old, "") == "fragile_suit" &&
              GuestPickFromSettings(old, "cliff") == "cliff" && GuestPickFromSettings(old, "fragile") == "fragile" &&
              GuestPickFromSettings(old, "gandalf") == "fragile_suit" && GuestPickFromSettings(old, "Sam") == "fragile_suit" &&
              GuestPickFromSettings(old, "Cliff") == "cliff");
        old["character_defaults"] = kCharacterDefaults;
        CHECK(GuestPickFromSettings(old, "sam") == "sam" && GuestPickFromSettings(old, "") == "fragile_suit");
    }

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
    // Vehicle hits: the same pattern (default on, only partner_vehicle_damage=0 written).
    CHECK(text.find("partner_vehicle_damage") == std::string::npos && back.partner_vehicle_damage);
    host.partner_vehicle_damage = false;
    CHECK(MakeSessionIniText(host, text, error) && text.find("partner_vehicle_damage=0\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.partner_vehicle_damage);
    CHECK(ParseSessionIniText(text + "partner_vehicle_damage=1\n", back, error) && back.partner_vehicle_damage);
    host.partner_vehicle_damage = true;
    // Dev tools (the Debug page's checkbox): off by default and then not written; dev_tools=1 when set, parsed back; the
    // Debug options reach every session through ApplyDebugOptions (the Local test's two games too).
    CHECK(text.find("dev_tools") == std::string::npos && !back.dev_tools);
    host.dev_tools = true;
    CHECK(MakeSessionIniText(host, text, error) && text.find("dev_tools=1\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.dev_tools);
    host.dev_tools = false;
    {
        DebugOptions dbg;
        CHECK(!dbg.dev_tools);
        dbg.dev_tools = true;
        SessionIni applied;
        ApplyDebugOptions(dbg, applied);
        CHECK(applied.dev_tools);
    }
    // Self voice (docs/audio-research.md Phase 1): auto by default, written for every role, parsed back; only
    // auto / sam / mute; a file without it parses back as "" (the mod keeps Sam's voice).
    CHECK(SessionIni{}.self_voice == "auto");
    CHECK(MakeSessionIniText(host, text, error) && text.find("self_voice=auto\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.self_voice == "auto");
    guest.my_character = "fragile_suit";
    guest.self_voice = "sam";
    CHECK(MakeSessionIniText(guest, text, error) && text.find("self_voice=sam\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.self_voice == "sam");
    guest.self_voice = "mute";
    CHECK(MakeSessionIniText(guest, text, error) && text.find("self_voice=mute\n") != std::string::npos);
    guest.self_voice = "Mute";
    CHECK(!MakeSessionIniText(guest, text, error) && error.find("self_voice") != std::string::npos);
    guest.self_voice = "fragile";
    CHECK(!MakeSessionIniText(guest, text, error));
    guest.self_voice.clear();
    CHECK(MakeSessionIniText(guest, text, error) && text.find("self_voice") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.self_voice.empty());
    CHECK(!ParseSessionIniText(text + "self_voice=loud\n", back, error) && error.find("self_voice") != std::string::npos);
    // Partner voice (docs/audio-research.md Phase 2): voice_sync=1 written only when set, parsed back; absent = off.
    CHECK(!SessionIni{}.voice_sync);
    CHECK(MakeSessionIniText(guest, text, error) && text.find("voice_sync") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.voice_sync);
    guest.voice_sync = true;
    CHECK(MakeSessionIniText(guest, text, error) && text.find("voice_sync=1\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.voice_sync);
    host.voice_sync = true;
    CHECK(MakeSessionIniText(host, text, error) && text.find("voice_sync=1\n") != std::string::npos);
    host.voice_sync = false;
    // Partner sounds (docs/audio-research.md Phase 3): sound_sync=1 written only when set, parsed back; absent = off.
    CHECK(!SessionIni{}.sound_sync);
    CHECK(MakeSessionIniText(guest, text, error) && text.find("sound_sync") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.sound_sync);
    guest.sound_sync = true;
    CHECK(MakeSessionIniText(guest, text, error) && text.find("sound_sync=1\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.sound_sync);
    guest.sound_sync = false;
    host.sound_sync = true;
    CHECK(MakeSessionIniText(host, text, error) && text.find("sound_sync=1\n") != std::string::npos);
    host.sound_sync = false;
    // Partner sound volume (Phase 4): written only with sound_sync and below 100, parsed back; refused outside 0..100.
    CHECK(SessionIni{}.partner_volume == 100);
    guest.sound_sync = true;
    CHECK(MakeSessionIniText(guest, text, error) && text.find("partner_volume") == std::string::npos);
    guest.partner_volume = 40;
    CHECK(MakeSessionIniText(guest, text, error) && text.find("partner_volume=40\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.partner_volume == 40);
    guest.partner_volume = 0;
    CHECK(MakeSessionIniText(guest, text, error) && text.find("partner_volume=0\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.partner_volume == 0);
    guest.sound_sync = false;
    CHECK(MakeSessionIniText(guest, text, error) && text.find("partner_volume") == std::string::npos);  // no partner sounds: not written
    guest.sound_sync = true;
    guest.partner_volume = 101;
    CHECK(!MakeSessionIniText(guest, text, error));
    guest.partner_volume = -1;
    CHECK(!MakeSessionIniText(guest, text, error));
    CHECK(!ParseSessionIniText("partner_volume=150\n", back, error) && !ParseSessionIniText("partner_volume=x\n", back, error));
    guest.partner_volume = 100;
    guest.sound_sync = false;
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
    CHECK(StatusViewOf(s, true, "guest").features_off.empty());   // an older mod / everything on: no key
    {
        json::Value off = s;
        off["features_off"] = "host_save=0 (the launcher's Debug > Host holds the save is off)";
        CHECK(StatusViewOf(off, true, "host").features_off.find("host_save=0") == 0);
    }

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
    // The host-held save: on by default (also for settings from before it); off is remembered.
    CHECK(DebugOptions{}.host_save && DebugOptions{}.backup_host_saves && DebugOptionsFromSettings(json::Value::Object()).host_save &&
          DebugOptionsFromSettings(json::Value::Object()).backup_host_saves && r.host_save && r.backup_host_saves);
    {
        DebugOptions off;
        off.host_save = false;
        off.backup_host_saves = false;
        json::Value s = json::Value::Object(), sb;
        DebugOptionsToSettings(off, s);
        CHECK(json::Parse(json::Dump(s), sb) && !DebugOptionsFromSettings(sb).host_save && !DebugOptionsFromSettings(sb).backup_host_saves);
    }
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
    // Other characters for the guest: off by default (older settings files too), saved, never in session.ini.
    CHECK(!DebugOptions{}.allow_other_characters && !DebugOptionsFromSettings(json::Value::Object()).allow_other_characters);
    {
        DebugOptions a;
        a.allow_other_characters = true;
        json::Value s = json::Value::Object(), sb;
        DebugOptionsToSettings(a, s);
        CHECK(json::Parse(json::Dump(s), sb) && DebugOptionsFromSettings(sb).allow_other_characters);
    }
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
    // Vehicle hits: on by default (older settings files too), saved, copied into the session.ini.
    CHECK(DebugOptions{}.partner_vehicle_damage && DebugOptionsFromSettings(json::Value::Object()).partner_vehicle_damage && ini.partner_vehicle_damage);
    d.partner_vehicle_damage = false;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions vd = DebugOptionsFromSettings(back);
    CHECK(!vd.partner_vehicle_damage);
    ApplyDebugOptions(vd, ini);
    CHECK(!ini.partner_vehicle_damage);
    d.partner_vehicle_damage = true;
    // Self voice: muting on by default (older settings files too: self_voice=auto), saved as auto / sam, copied in.
    CHECK(DebugOptions{}.mute_sam_voice && DebugOptionsFromSettings(json::Value::Object()).mute_sam_voice && ini.self_voice == "auto");
    d.mute_sam_voice = false;
    DebugOptionsToSettings(d, settings);
    CHECK(settings.Find("debug")->Str("self_voice") == "sam");
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions sv = DebugOptionsFromSettings(back);
    CHECK(!sv.mute_sam_voice);
    ApplyDebugOptions(sv, ini);
    CHECK(ini.self_voice == "sam");
    d.mute_sam_voice = true;
    DebugOptionsToSettings(d, settings);
    CHECK(settings.Find("debug")->Str("self_voice") == "auto" && json::Parse(json::Dump(settings), back));
    ApplyDebugOptions(DebugOptionsFromSettings(back), ini);
    CHECK(ini.self_voice == "auto");
    // Partner voice: on by default (older settings files too), saved as a bool, copied in.
    CHECK(DebugOptions{}.partner_voice_sync && DebugOptionsFromSettings(json::Value::Object()).partner_voice_sync && ini.voice_sync);
    d.partner_voice_sync = false;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions vs = DebugOptionsFromSettings(back);
    CHECK(!vs.partner_voice_sync);
    ApplyDebugOptions(vs, ini);
    CHECK(!ini.voice_sync);
    d.partner_voice_sync = true;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    ApplyDebugOptions(DebugOptionsFromSettings(back), ini);
    CHECK(ini.voice_sync);
    // Partner sounds: on by default (older settings files too), saved as a bool, copied in.
    CHECK(DebugOptions{}.partner_sound_sync && DebugOptionsFromSettings(json::Value::Object()).partner_sound_sync && ini.sound_sync);
    d.partner_sound_sync = false;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    const DebugOptions ss = DebugOptionsFromSettings(back);
    CHECK(!ss.partner_sound_sync);
    ApplyDebugOptions(ss, ini);
    CHECK(!ini.sound_sync);
    d.partner_sound_sync = true;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    ApplyDebugOptions(DebugOptionsFromSettings(back), ini);
    CHECK(ini.sound_sync);
    // Partner sound volume: 100 by default (older settings files too), saved, copied in; out-of-range falls back.
    CHECK(DebugOptions{}.partner_volume == 100 && DebugOptionsFromSettings(json::Value::Object()).partner_volume == 100 && ini.partner_volume == 100);
    d.partner_volume = 35;
    DebugOptionsToSettings(d, settings);
    CHECK(json::Parse(json::Dump(settings), back));
    ApplyDebugOptions(DebugOptionsFromSettings(back), ini);
    CHECK(ini.partner_volume == 35);
    d.partner_volume = 100;
    json::Value vol;
    CHECK(json::Parse("{\"debug\":{\"partner_volume\":1e300}}", vol) && DebugOptionsFromSettings(vol).partner_volume == 100);
    CHECK(json::Parse("{\"debug\":{\"partner_volume\":-5}}", vol) && DebugOptionsFromSettings(vol).partner_volume == 100);
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
    // The local test's two files for the testenv (-LauncherIni): only into --out, never the game folder.
    const ui::Options lo = ui::ParseCommandLine({L"--write-session", L"local-test", L"--out", L"C:\\x\\launcher-ini", L"--game-dir", L"C:\\g"});
    CHECK(lo.error.empty() && lo.write_role == L"local-test" && lo.write_out == L"C:\\x\\launcher-ini");
    CHECK(!ui::ParseCommandLine({L"--write-session", L"local-test"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--write-session", L"host", L"--out", L"C:\\x"}).error.empty());
    CHECK(!ui::ParseCommandLine({L"--out", L"C:\\x"}).error.empty());
    CHECK(ui::SceneCount() == static_cast<int>(ui::Scene::count) && ui::SceneCount() == 45);
    CHECK(std::string(ui::SceneName(ui::Scene::debug_options_coop)) == "41-debug-options-coop" &&
          std::string(ui::SceneName(ui::Scene::debug_local_test_off)) == "42-debug-local-test-off");
    CHECK(std::string(ui::SceneName(ui::Scene::debug_test_saves)) == "36-debug-test-saves" &&
          std::string(ui::SceneName(ui::Scene::debug_local_test)) == "37-debug-local-test" &&
          std::string(ui::SceneName(ui::Scene::play_join_characters)) == "38-play-join-characters" &&
          std::string(ui::SceneName(ui::Scene::play_join_hologram)) == "39-play-join-hologram" &&
          std::string(ui::SceneName(ui::Scene::debug_options_characters)) == "40-debug-options-characters");
    CHECK(std::string(ui::DebugTabLabel(ui::DebugTab::saves)) == "Test saves" && ui::kDebugTabCount == 3);
    for (int i = 0; i < ui::SceneCount(); ++i) CHECK(*ui::SceneName(static_cast<ui::Scene>(i)) != '\0');
}

void MockFlow() {
    MockModel m(MockOptions{});
    m.use_manual_clock(0.0);
    CHECK(m.my_character() == "fragile_suit" && m.local_test().guest_character == "fragile_suit");
    m.set_my_character("Fragile");   // fixed without "Allow other characters"
    CHECK(m.my_character() == "fragile_suit");
    {
        DebugOptions a;
        a.allow_other_characters = true;
        m.set_debug_options(a);
    }
    m.set_my_character("Fragile");
    m.set_local_test_guest_character("gandalf");   // not a character
    CHECK(m.my_character() == "fragile" && m.local_test().guest_character == "fragile_suit");
    m.set_debug_options(DebugOptions{});
    CHECK(m.my_character() == "fragile_suit");
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
    CHECK(ini.my_character == "sam" && ini.partner_body == "real" && m->my_character() == "fragile_suit");   // the host is Sam
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
    m->set_my_character("diehardman");   // fixed without "Allow other characters"
    CHECK(m->my_character() == "fragile_suit");
    {
        DebugOptions a;
        a.allow_other_characters = true;
        m->set_debug_options(a);
    }
    m->set_my_character("diehardman");
    CHECK(m->my_character() == "diehardman");
    CHECK(m->join("+stranded_join " + partner, error) && m->session_role() == Role::join);
    CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && ini.role == "guest" &&
          ini.peer_steamid == Decimal(kSteamIdBase + 7919ull) && ini.secret == "7QK2MX9RT4PB8WZ3HCNA");
    CHECK(ini.my_character == "diehardman" && ini.partner_body == "real");
    CHECK(ini.self_character && text.find("self_character=1\n") != std::string::npos);   // the default: see yourself as your character
    CHECK(ini.self_voice == "auto" && text.find("self_voice=auto\n") != std::string::npos);  // the default: Sam's voice muted for a non-Sam
    CHECK(ini.voice_sync && text.find("voice_sync=1\n") != std::string::npos);  // the default: each hears the other's voice
    CHECK(ini.sound_sync && text.find("sound_sync=1\n") != std::string::npos);  // the default: each hears the other's sounds
    CHECK(ini.partner_volume == 100 && text.find("partner_volume") == std::string::npos);  // the default: full volume, no key

    CHECK(m->cancel_launch(error) && m->phase() == Phase::ready && !IsFile(paths.session_ini));
    m->set_my_character("gandalf");   // not in the mod's table: Fragile (porter suit)
    CHECK(m->my_character() == "fragile_suit");
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
        CHECK(again->my_character() == "fragile_suit");   // "Allow other characters" is off again
        DebugOptions a;
        a.allow_other_characters = true;
        again->set_debug_options(a);
        CHECK(again->my_character() == "fragile");   // the stored pick comes back
        again->set_debug_options(DebugOptions{});
    }
    {
        // Settings from before Fragile (porter suit) was the guest's character: the old default "sam" is
        // migrated, a real pick is kept (and counts only with "Allow other characters").
        std::string saved;
        CHECK(ReadTextShared(paths.settings, saved));
        CHECK(WriteAtomic(paths.settings,
                          std::string("{\"product\":\"stranded-together-launcher-settings\",\"v\":1,\"my_character\":\"sam\",\"test_save\":{\"guest_character\":\"cliff\"},"
                                      "\"debug\":{\"allow_other_characters\":true}}"),
                          true, error));
        auto old = MakeRealModelWithEnv(dir, env);
        CHECK(old->my_character() == "fragile_suit" && old->local_test().guest_character == "cliff");
        old->set_player_name("Sam");   // any save writes the marker
        std::string now_text;
        json::Value now;
        CHECK(ReadTextShared(paths.settings, now_text) && json::Parse(now_text, now) && now.Int("character_defaults", 0) == kCharacterDefaults &&
              now.Str("my_character") == "fragile_suit");
        old.reset();
        auto reread = MakeRealModelWithEnv(dir, env);   // migrated once: still Fragile (porter suit), "sam" does not come back
        CHECK(reread->my_character() == "fragile_suit" && reread->local_test().guest_character == "cliff");
        CHECK(WriteAtomic(paths.settings, saved, true, error));
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
        m->set_local_test_guest_character("cliff");   // fixed without "Allow other characters"
        CHECK(m->local_test().guest_character == "fragile_suit");
        {
            DebugOptions a = m->debug_options();
            a.allow_other_characters = true;
            m->set_debug_options(a);
        }
        m->set_local_test_guest_character("cliff");
        CHECK(m->local_test().guest_character == "cliff");
        // v0.23: the frame rate choice (60 by default; 30 / 60 / 120 / 0 = the game's maximum, anything else the default)
        CHECK(m->local_test().fps == 60);
        m->set_local_test_fps(7);
        CHECK(m->local_test().fps == 60);
        m->set_local_test_fps(120);
        CHECK(m->local_test().fps == 120);
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
              ini.frame_cap == 120 && ini.debug_menu && ini.local_window == "left");
        CHECK(ini.my_character == "sam" && ini.partner_body == "real" && !ini.self_character);   // the host game is always Sam
        const std::string secret = ini.secret;
        CHECK(ReadTextShared(guest_ini, text) && ParseSessionIniText(text, ini, error));
        CHECK(ini.role == "guest" && ini.udp_port_local == 27556 && ini.udp_port_peer == 27555 && ini.sandbox_saves == "local-guest" &&
              ini.second_instance && ini.local_input == "guest" && ini.secret == secret && ini.local_window == "right");
        CHECK(ini.my_character == "cliff" && ini.partner_body == "real" && ini.self_character);   // the guest sees itself as Cliff
        // v0.19: the local test plays the online session (host_save=1, the Debug default): the guest's waiting screen and
        // the save transfer, and on them the shared vehicles, riding together and vehicle hits. v0.18.0 wrote no host_save,
        // so all four were off in the user's play test and nothing said so.
        CHECK(ini.host_save && ini.sandbox_foreign_saves && text.find("host_save=1\n") != std::string::npos);
        CHECK(ini.mouse_lock && text.find("mouse_lock=1\n") != std::string::npos);   // the mouse lock (Debug default on)
        for (const char* off : {"partner_vehicle_damage=0", "partner_collision=0", "ride_together", "vehicle_sync"})
            CHECK(text.find(off) == std::string::npos);
        {
            SessionIni hi;
            std::string ht;
            CHECK(ReadTextShared(host_ini, ht) && ParseSessionIniText(ht, hi, error) && hi.host_save && hi.sandbox_saves == "local-host" &&
                  hi.sandbox_foreign_saves && ht.find("host_save=1\n") != std::string::npos);
        }
        // The guest's folder is prepared like coop-guest: its own profile, no save slot (the host's save arrives over the
        // link); the host plays its local-host copy of the test save: no backup of the real saves is made.
        {
            const std::wstring gacct = JoinPath2(GetSandboxPaths(paths, kLocalGuestSandbox).saves, L"DeathStrandingDC\\104729");
            CHECK(IsFile(JoinPath2(gacct, L"profile\\profile.dat")) && !IsDir(JoinPath2(gacct, L"autosave3")) && !IsDir(JoinPath2(gacct, L"manualsave1")));
            CHECK(SandboxIsSessionGuest(GetSandboxPaths(paths, kLocalGuestSandbox)));
            CHECK(IsFile(JoinPath2(GetSandboxPaths(paths, kLocalHostSandbox).saves, L"DeathStrandingDC\\104729\\autosave3\\checkpoint.dat")));
            CHECK(!IsDir(HostBackupsDir(paths)));
        }
        CHECK(SandboxSeeded(GetSandboxPaths(paths, kLocalHostSandbox)) && SandboxSeeded(GetSandboxPaths(paths, kLocalGuestSandbox)));
        CHECK(IsFile(GetSandboxPaths(paths, kLocalGuestSandbox).settings));
        for (const char* sb : {kLocalHostSandbox, kLocalGuestSandbox}) {
            std::string cfg;
            CHECK(ReadTextShared(GetSandboxPaths(paths, sb).settings, cfg) && cfg.find("\"fps\" \"120\"") != std::string::npos &&
                  cfg.find("\"limit_fps\" \"120\"") != std::string::npos && cfg.find("\"fps\" \"30\"") == std::string::npos);
        }
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
        // Debug > Host holds the save off: the old local test (each game on its own copy of the test save); the guest
        // folder the host-save test left without slots gets its copy back. The game window says what that costs.
        {
            DebugOptions a = m->debug_options();
            a.host_save = false;
            m->set_debug_options(a);
            CHECK(m->start_local_test(error));
            CHECK(ReadTextShared(guest_ini, text) && ParseSessionIniText(text, ini, error) && !ini.host_save && text.find("host_save") == std::string::npos);
            CHECK(ReadTextShared(host_ini, text) && ParseSessionIniText(text, ini, error) && !ini.host_save);
            const SandboxPaths lg = GetSandboxPaths(paths, kLocalGuestSandbox);
            CHECK(!SandboxIsSessionGuest(lg) && IsFile(JoinPath2(lg.saves, L"DeathStrandingDC\\104729\\autosave3\\checkpoint.dat")));
            env->now += 9000;
            m->tick();
            // what a game reports off reaches the local test's card
            WriteFile(JoinPath2(paths.mod, L"instances\\guest\\link-status.json"),
                      "{\"v\":1,\"pid\":" + std::to_string(m->local_test().guest_pid) +
                          ",\"state\":\"connected\",\"features_off\":\"host_save=0 (test)\",\"time_utc\":\"2026-01-01T00:00:00Z\"}");
            CHECK(m->local_test().guest_pid != 0);
            env->now += 1500;
            m->tick();
            CHECK(m->local_test().features_off.find("Guest game: host_save=0 (test)") != std::string::npos);
            env->alive.clear();
            m->tick();
            env->now += 4000;
            m->tick();
            CHECK(!m->local_test().running && m->local_test().features_off.empty());
            a.host_save = true;
            m->set_debug_options(a);
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
    // v0.23: 60 fps by default (was 30), the game's own rewrite of limit_fps replaced; the maximum is 240, never 0 (the game
    // turns 0 into 15); the hidden env's plain "graphics low" text stays at 30.
    CHECK(fresh.find("\"fps\" \"60\"\r\n") != std::string::npos && fresh.find("\"limit_fps\" \"60\"\r\n") != std::string::npos);
    CHECK(fresh.find("\"fps\" \"30\"") == std::string::npos && fresh.find("\"limit_fps\" \"30\"") == std::string::npos);
    const std::string rewritten = LocalTestSettingsCfg("\"wait_vsync\" \"1\"\r\n\"fps\" \"30\"\r\n\"limit_fps\" \"30\"\r\n", 120);
    CHECK(rewritten.find("\"limit_fps\" \"120\"") != std::string::npos && rewritten.find("\"fps\" \"120\"") != std::string::npos &&
          rewritten.find("\"30\"") == std::string::npos);
    CHECK(LocalTestSettingsCfg("", LocalTestGameFps(0)).find("\"fps\" \"240\"") != std::string::npos);
    CHECK(LocalTestSettingsCfg("", 5).find("\"fps\" \"15\"") != std::string::npos);
    CHECK(std::string(st::sandbox::LowGraphicsSettingsCfg()).find("\"fps\" \"30\"") != std::string::npos);
    CHECK(LocalTestFps(30) == 30 && LocalTestFps(60) == 60 && LocalTestFps(120) == 120 && LocalTestFps(0) == 0 && LocalTestFps(45) == 60);
    CHECK(LocalTestGameFps(0) == 240 && LocalTestGameFps(120) == 120 && LocalTestGameFps(-3) == 60 && LocalTestFrameCap(0) == 0);
    LocalTestChoices c;
    SessionIni h, g;
    MakeLocalTestInis(DebugOptions{}, c, h, g);
    CHECK(h.frame_cap == 60 && g.frame_cap == 60);
    c.fps = 0;
    MakeLocalTestInis(DebugOptions{}, c, h, g);
    CHECK(h.frame_cap == 0 && g.frame_cap == 0);
}

// The mouse lock (play test 2: the cursor walked out of the game window into the other one): a Debug option, on by
// default, written as mouse_lock=1 into BOTH Local test files (MakeLocalTestInis, the files the button and the hidden
// `launcherini local-test` runs use), never by Host / Join (ApplyDebugOptions), parsed strictly.
void MouseLockSettings() {
    CHECK(DebugOptions{}.mouse_lock && DebugOptionsFromSettings(json::Value::Object()).mouse_lock);
    {
        DebugOptions off;
        off.mouse_lock = false;
        json::Value s = json::Value::Object(), sb;
        DebugOptionsToSettings(off, s);
        CHECK(json::Parse(json::Dump(s), sb) && !DebugOptionsFromSettings(sb).mouse_lock);
        DebugOptionsToSettings(DebugOptions{}, s);
        CHECK(json::Parse(json::Dump(s), sb) && DebugOptionsFromSettings(sb).mouse_lock);
    }
    LocalTestChoices choices;
    choices.secret = "K7RD3WQ9MP2XVB6TZHN4";
    SessionIni host, guest, back;
    std::string text, error;
    MakeLocalTestInis(DebugOptions{}, choices, host, guest);
    CHECK(host.mouse_lock && guest.mouse_lock);
    CHECK(MakeSessionIniText(host, text, error) && text.find("mouse_lock=1\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.mouse_lock);
    CHECK(MakeSessionIniText(guest, text, error) && text.find("mouse_lock=1\n") != std::string::npos);
    // world sync (docs/world-sync.md): on in both Local test files (the Debug option, default on), parsed back, strict
    CHECK(host.world_sync && guest.world_sync && text.find("world_sync=1\n") != std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.world_sync);
    CHECK(!ParseSessionIniText(text + "world_sync=yes\n", back, error));
    CHECK(ParseSessionIniText(text, back, error) && back.mouse_lock && back.local_window == "right");
    DebugOptions off;
    off.mouse_lock = false;
    MakeLocalTestInis(off, choices, host, guest);
    CHECK(!host.mouse_lock && !guest.mouse_lock);
    CHECK(MakeSessionIniText(guest, text, error) && text.find("mouse_lock") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && !back.mouse_lock);
    // Host / Join (ApplyDebugOptions): never written, whatever the option says (one game, the game's own cursor handling).
    SessionIni online;
    online.role = "host";
    online.secret = choices.secret;
    ApplyDebugOptions(DebugOptions{}, online);
    CHECK(!online.mouse_lock && MakeSessionIniText(online, text, error) && text.find("mouse_lock") == std::string::npos);
    CHECK(!online.world_sync && text.find("world_sync") == std::string::npos);   // without host_save: not written (the mod: off)
    // W3: "Share structures, ladders and ropes" (default on): the Local test writes it; off -> world_sync=0 with host_save
    // (the mod's own default is on with host_save=1), and it round-trips through the settings.
    {
        DebugOptions so;
        CHECK(so.share_world && so.host_save);
        so.share_world = false;
        MakeLocalTestInis(so, choices, host, guest);
        CHECK(!host.world_sync && !guest.world_sync && host.host_save);
        CHECK(MakeSessionIniText(host, text, error) && text.find("world_sync=0\n") != std::string::npos);
        CHECK(ParseSessionIniText(text, back, error) && !back.world_sync);
        json::Value sset = json::Value::Object(), sb;
        DebugOptionsToSettings(so, sset);
        CHECK(json::Parse(json::Dump(sset), sb) && !DebugOptionsFromSettings(sb).share_world);
        SessionIni hs;
        hs.role = "host";
        hs.secret = choices.secret;
        hs.host_save = true;
        hs.world_sync = true;
        CHECK(MakeSessionIniText(hs, text, error) && text.find("world_sync=1\n") != std::string::npos);
    }
    // Strict: 0 / 1 only.
    CHECK(ParseSessionIniText(text + "mouse_lock=0\n", back, error) && !back.mouse_lock);
    CHECK(ParseSessionIniText(text + "mouse_lock=1\n", back, error) && back.mouse_lock);
    CHECK(!ParseSessionIniText(text + "mouse_lock=yes\n", back, error) && error.find("mouse_lock") != std::string::npos);
    CHECK(!ParseSessionIniText(text + "mouse_lock=2\n", back, error));
}

// The host-held save (M1): session.ini's refuse rule, the guest's coop-guest folder, the host's backups and the
// non-slot manifest, and the real model's Host / Join with the Debug option on and off. On a fake save root only.
void HostSaveSession() {
    const std::wstring local = NewTempDir(L"st-launcher-hs-local-");
    wchar_t old_local[1024]{};
    const DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", old_local, 1024);
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    const std::wstring real = JoinPath2(local, L"KojimaProductions");
    const std::wstring acct = JoinPath2(real, L"DeathStrandingDC\\104729");
    WriteFile(JoinPath2(acct, L"profile\\profile.dat"), "own profile");
    WriteFile(JoinPath2(acct, L"profile\\game_settings.cfg"), "\"GameDifficulty\" \"4\"\r\n");
    WriteFile(JoinPath2(acct, L"bindings.cfg"), "own bindings");
    WriteFile(JoinPath2(acct, L"autosave0\\checkpoint.dat"), "own slot 0");
    WriteFile(JoinPath2(acct, L"manualsave1\\checkpoint.dat"), "own manual 1");
    WriteFile(JoinPath2(acct, L"steam_autocloud.vdf"), "steam");
    WriteFile(JoinPath2(real, L"DeathStrandingDC\\555\\profile\\profile.dat"), "another account's profile");
    const std::string real_before = SaveTreeManifest(real);

    // ---- session.ini: the refuse rule (a host-save guest only with the private folder's keys) ----
    CHECK(std::string(kCoopGuestSandbox) == st::sandbox::kCoopGuestSandbox);
    std::string text, error;
    SessionIni g;
    g.role = "guest";
    g.secret = "K7RD3WQ9MP2XVB6TZHN4";
    g.peer_steamid = "76561197960370457";
    g.host_save = true;
    CHECK(!MakeSessionIniText(g, text, error) && error.find("coop-guest") != std::string::npos && text.empty());   // no sandbox at all
    g.sandbox_saves = kCoopGuestSandbox;
    CHECK(!MakeSessionIniText(g, text, error));   // without sandbox_foreign_saves
    g.sandbox_saves = "test";
    g.sandbox_foreign_saves = true;
    CHECK(!MakeSessionIniText(g, text, error));   // another sandbox name
    g.sandbox_saves = kCoopGuestSandbox;
    CHECK(MakeSessionIniText(g, text, error));
    CHECK(text.find("host_save=1\n") != std::string::npos && text.find("sandbox_saves=coop-guest\n") != std::string::npos &&
          text.find("sandbox_foreign_saves=1\n") != std::string::npos);
    SessionIni back;
    CHECK(ParseSessionIniText(text, back, error) && back.host_save && back.sandbox_saves == "coop-guest" && back.sandbox_foreign_saves);
    g.host_save = false;   // the old mode: no key
    CHECK(MakeSessionIniText(g, text, error) && text.find("host_save") == std::string::npos);
    SessionIni h;
    h.role = "host";
    h.secret = "K7RD3WQ9MP2XVB6TZHN4";
    h.host_save = true;   // the host plays on its own saves: no sandbox needed
    CHECK(MakeSessionIniText(h, text, error) && text.find("host_save=1\n") != std::string::npos && text.find("sandbox") == std::string::npos);
    CHECK(ParseSessionIniText(text, back, error) && back.host_save && back.role == "host");
    for (const char* role : {"echo", "solo"}) {
        SessionIni other;
        other.role = role;
        other.host_save = true;
        CHECK(!MakeSessionIniText(other, text, error) && error.find("host_save") != std::string::npos);
    }
    // The local test's guest game runs the host-held save on its own sandbox local-guest (prepared like coop-guest);
    // never on the host's local-host copy, a test-save folder or the real saves.
    CHECK(std::string(kLocalTestGuestSandbox) == kLocalGuestSandbox && IsHostSaveGuestSandbox("local-guest") &&
          IsHostSaveGuestSandbox("coop-guest") && !IsHostSaveGuestSandbox("local-host") && !IsHostSaveGuestSandbox("") &&
          !IsHostSaveGuestSandbox("test"));
    {
        SessionIni lg = g;
        lg.host_save = true;
        lg.transport = "udp";
        lg.udp_peer = "127.0.0.1";
        lg.udp_port_local = 27556;
        lg.udp_port_peer = 27555;
        lg.peer_steamid.clear();
        lg.sandbox_foreign_saves = true;
        lg.second_instance = true;
        lg.sandbox_saves = kLocalGuestSandbox;
        CHECK(MakeSessionIniText(lg, text, error) && text.find("host_save=1\n") != std::string::npos);
        lg.sandbox_saves = kLocalHostSandbox;
        CHECK(!MakeSessionIniText(lg, text, error) && error.find("local-guest") != std::string::npos);
    }

    // ---- MakeLocalTestInis: what the Local test button writes (the launcher's defaults) ----
    {
        SessionIni lh, lgu;
        LocalTestChoices ch;
        ch.guest_character = "";   // the default pick
        MakeLocalTestInis(DebugOptions{}, ch, lh, lgu);
        std::string ht, gt;
        CHECK(MakeSessionIniText(lh, ht, error) && MakeSessionIniText(lgu, gt, error));
        // both games: the host-held save session and, on it, the shared vehicles (vehicle_sync default 5 with host_save),
        // riding together (ride_together default 1) and vehicle hits (partner_vehicle_damage default 1): no key turns
        // one of them off
        for (const std::string* t : {&ht, &gt}) {
            CHECK(t->find("host_save=1\n") != std::string::npos && t->find("transport=udp\n") != std::string::npos &&
                  t->find("sandbox_foreign_saves=1\n") != std::string::npos && t->find("partner_body=real\n") != std::string::npos &&
                  t->find("one_shot=1\n") != std::string::npos && t->find("debug_menu=1\n") != std::string::npos);
            for (const char* off : {"vehicle_sync", "ride_together", "partner_vehicle_damage=0", "partner_collision=0", "automation"})
                CHECK(t->find(off) == std::string::npos);
        }
        CHECK(ht.find("role=host\n") != std::string::npos && ht.find("sandbox_saves=local-host\n") != std::string::npos &&
              ht.find("my_character=sam\n") != std::string::npos && ht.find("local_input=host\n") != std::string::npos &&
              ht.find("local_window=left\n") != std::string::npos && ht.find("second_instance") == std::string::npos);
        CHECK(gt.find("role=guest\n") != std::string::npos && gt.find("sandbox_saves=local-guest\n") != std::string::npos &&
              gt.find("my_character=fragile_suit\n") != std::string::npos && gt.find("self_character=1\n") != std::string::npos &&
              gt.find("second_instance=1\n") != std::string::npos && gt.find("local_input=guest\n") != std::string::npos &&
              gt.find("udp_port_local=27556\n") != std::string::npos && gt.find("udp_port_peer=27555\n") != std::string::npos);
        CHECK(lh.secret == lgu.secret && IsCanonicalSecret(lh.secret));
        DebugOptions old_mode;
        old_mode.host_save = false;
        MakeLocalTestInis(old_mode, ch, lh, lgu);
        CHECK(!lh.host_save && !lgu.host_save && MakeSessionIniText(lgu, gt, error) && gt.find("host_save") == std::string::npos);
    }

    // ---- NonSlotManifest ----
    CHECK(IsSaveSlotName(L"autosave0") && IsSaveSlotName(L"ManualSave12") && !IsSaveSlotName(L"profile") && !IsSaveSlotName(L"autosave"));
    const std::string ns = NonSlotManifest(SaveTreeManifest(real));
    CHECK(ns.find("profile.dat") != std::string::npos && ns.find("bindings.cfg") != std::string::npos);
    CHECK(ns.find("autosave0") == std::string::npos && ns.find("manualsave1") == std::string::npos);
    CHECK(NonSlotManifest("a\\autosave3\\checkpoint.dat\t5\tX\nb\\profile\\p.dat\t1\tY\na\\autosave3\\\t-\t-\nquicksave0x\\f\t1\tZ\n") ==
          "b\\profile\\p.dat\t1\tY\nquicksave0x\\f\t1\tZ\n");

    // ---- SeedSessionGuest ----
    const std::wstring dir = NewTempDir(L"st-launcher-hs-game-");
    CHECK(WriteAtomic(JoinPath2(dir, L"ds.exe"), std::string(), true, error));
    CHECK(WriteAtomic(JoinPath2(dir, L"winhttp.dll"), FakeOurDll(), true, error));
    const InstancePaths paths = GetInstancePaths(dir);
    const SandboxPaths cg = GetSandboxPaths(paths, kCoopGuestSandbox);
    SessionGuestReport rep;
    CHECK(!SeedSessionGuest(paths, real, 0, rep, error) && !SandboxSeeded(cg));   // the account must be known
    CHECK(SeedSessionGuest(paths, real, 104729, rep, error) && SandboxSeeded(cg));
    const std::wstring gacct = JoinPath2(cg.saves, L"DeathStrandingDC\\104729");
    CHECK(ReadTextShared(JoinPath2(gacct, L"profile\\profile.dat"), text) && text == "own profile");
    CHECK(ReadTextShared(JoinPath2(gacct, L"bindings.cfg"), text) && text == "own bindings");
    CHECK(IsFile(JoinPath2(gacct, L"profile\\game_settings.cfg")) && !IsFile(JoinPath2(gacct, L"steam_autocloud.vdf")));
    CHECK(!IsDir(JoinPath2(gacct, L"autosave0")) && !IsDir(JoinPath2(gacct, L"manualsave1")));   // never the guest's own slots
    CHECK(!IsDir(JoinPath2(cg.saves, L"DeathStrandingDC\\555")));   // never another account's profile
    CHECK(rep.copied == 2 && rep.wiped_slots == 0 && !rep.wiped_incoming);
    // A session ran: the host's save arrived, the guest's game autosaved, settings changed, another account appeared.
    WriteFile(JoinPath2(gacct, L"manualsave5\\checkpoint.dat"), "host save copy");
    WriteFile(JoinPath2(gacct, L"autosave2\\checkpoint.dat"), "guest autosave of the host's world");
    WriteFile(JoinPath2(cg.saves, L"incoming\\ab.part"), "partial");
    WriteFile(JoinPath2(gacct, L"bindings.cfg"), "bindings changed in co-op");
    WriteFile(JoinPath2(cg.saves, L"DeathStrandingDC\\31337\\profile\\profile.dat"), "stray account");
    CHECK(SeedSessionGuest(paths, real, 104729, rep, error) && SandboxSeeded(cg));
    CHECK(rep.wiped_slots == 2 && rep.wiped_incoming && rep.removed_accounts == 1 && rep.copied == 0);
    CHECK(!IsDir(JoinPath2(gacct, L"manualsave5")) && !IsDir(JoinPath2(gacct, L"autosave2")) && !IsDir(JoinPath2(cg.saves, L"incoming")));
    CHECK(!IsDir(JoinPath2(cg.saves, L"DeathStrandingDC\\31337")));
    CHECK(ReadTextShared(JoinPath2(gacct, L"bindings.cfg"), text) && text == "bindings changed in co-op");   // D1-6: kept in coop-guest
    CHECK(ReadTextShared(JoinPath2(acct, L"bindings.cfg"), text) && text == "own bindings");                // never the real tree
    CHECK(SaveTreeManifest(real) == real_before);
    // Never inside the real tree.
    CHECK(!SeedSessionGuest(paths, cg.root, 104729, rep, error));
    // A link below the co-op folder (the account folder, or DeathStrandingDC itself, pointing at the real one): refused
    // before anything is deleted; the real slots and the other accounts' folders stay.
    {
        WriteFile(JoinPath2(real, L"DeathStrandingDC\\555\\autosave1\\checkpoint.dat"), "another account's real save");
        const std::string real_with_555 = SaveTreeManifest(real);   // 555 is another (real) account with a slot now
        CHECK(RemoveTree(gacct) && MakeJunction(gacct, acct));
        CHECK(IsReparsePoint(gacct) && IsFile(JoinPath2(gacct, L"autosave0\\checkpoint.dat")));   // the real slot, through the link
        CHECK(!SeedSessionGuest(paths, real, 104729, rep, error) && error.find("link") != std::string::npos);
        CHECK(SaveTreeManifest(real) == real_with_555 && rep.wiped_slots == 0);
        CHECK(RemoveDirectoryW(gacct.c_str()) != 0 && SaveTreeManifest(real) == real_with_555);
        const std::wstring gdc = JoinPath2(cg.saves, L"DeathStrandingDC");
        CHECK(RemoveTree(gdc) && MakeJunction(gdc, JoinPath2(real, L"DeathStrandingDC")));
        CHECK(!SeedSessionGuest(paths, real, 104729, rep, error) && error.find("link") != std::string::npos);
        CHECK(SaveTreeManifest(real) == real_with_555 && rep.removed_accounts == 0);
        CHECK(RemoveDirectoryW(gdc.c_str()) != 0 && SaveTreeManifest(real) == real_with_555);
        CHECK(RemoveTree(JoinPath2(real, L"DeathStrandingDC\\555\\autosave1")) && SaveTreeManifest(real) == real_before);
        CHECK(SeedSessionGuest(paths, real, 104729, rep, error) && SandboxSeeded(cg));   // a normal folder again: seeded
    }

    // ---- BackupHostSaves ----
    HostBackupReport br;
    CHECK(!BackupHostSaves(paths, real, 104729, "not-a-stamp", 5, br, error));
    CHECK(!BackupHostSaves(paths, real, 0, "20261119-120000", 5, br, error));
    CHECK(BackupHostSaves(paths, real, 777, "20261119-120000", 5, br, error) && br.dir.empty());   // no saves yet: nothing to copy
    CHECK(BackupHostSaves(paths, real, 104729, "20261119-120000", 5, br, error));
    const std::wstring b0 = JoinPath2(HostBackupsDir(paths), L"20261119-120000");
    CHECK(IEquals(br.dir, b0) && br.slots == 2 && br.files == 5 && br.pruned == 0);
    CHECK(ReadTextShared(JoinPath2(b0, L"DeathStrandingDC\\104729\\autosave0\\checkpoint.dat"), text) && text == "own slot 0");
    CHECK(IsFile(JoinPath2(b0, L"DeathStrandingDC\\104729\\profile\\profile.dat")) && !IsFile(JoinPath2(b0, L"DeathStrandingDC\\104729\\steam_autocloud.vdf")));
    CHECK(ReadTextShared(JoinPath2(b0, L"backup.json"), text) && text.find("stranded-together-backup") != std::string::npos);
    CHECK(CheckSeedFolder(b0).ok && CheckSeedFolder(b0).slots == 2);   // a backup can be picked as a test save
    // The same second again: its own folder.
    CHECK(BackupHostSaves(paths, real, 104729, "20261119-120000", 5, br, error) && IEquals(br.dir, b0 + L"-2"));
    // Folders that are not ours are never pruned; half-done ones are cleared.
    WriteFile(JoinPath2(HostBackupsDir(paths), L"20200101-000000\\keep.txt"), "the user's own folder");
    WriteFile(JoinPath2(HostBackupsDir(paths), L"notes\\keep.txt"), "x");
    WriteFile(JoinPath2(HostBackupsDir(paths), L"20261119-110000.partial\\x"), "half done");
    for (int i = 1; i <= 6; ++i) CHECK(BackupHostSaves(paths, real, 104729, "20261119-12000" + std::to_string(i), 5, br, error));
    int ours = 0;
    for (const DirEntry& e : ListDirectory(HostBackupsDir(paths)))
        if (e.directory && IsFile(JoinPath2(JoinPath2(HostBackupsDir(paths), e.name), L"backup.json"))) ++ours;
    CHECK(ours == kHostBackupsKept);
    CHECK(!IsDir(b0) && !IsDir(b0 + L"-2") && IsDir(JoinPath2(HostBackupsDir(paths), L"20261119-120006")));   // the oldest went
    CHECK(IsFile(JoinPath2(HostBackupsDir(paths), L"20200101-000000\\keep.txt")) && IsFile(JoinPath2(HostBackupsDir(paths), L"notes\\keep.txt")));
    CHECK(!IsDir(JoinPath2(HostBackupsDir(paths), L"20261119-110000.partial")));
    CHECK(ReadTextShared(JoinPath2(JoinPath2(HostBackupsDir(paths), L"20261119-120006"), L"backup.json"), text) &&
          text.find("\"seq\":8") != std::string::npos);
    // The clock was set ahead once (five backups stamped 2030): a backup made after it was corrected is kept, the oldest
    // by seq goes, and the later-stamped ones that stay are counted for the log.
    CHECK(RemoveTree(HostBackupsDir(paths)));
    for (int i = 1; i <= 5; ++i) CHECK(BackupHostSaves(paths, real, 104729, "2030010" + std::to_string(i) + "-000000", 5, br, error));
    CHECK(BackupHostSaves(paths, real, 104729, "20261119-130000", 5, br, error));
    CHECK(IsDir(br.dir) && IEquals(br.dir, JoinPath2(HostBackupsDir(paths), L"20261119-130000")) && br.pruned == 1 && br.newer_named == 4);
    CHECK(!IsDir(JoinPath2(HostBackupsDir(paths), L"20300101-000000")) && IsDir(JoinPath2(HostBackupsDir(paths), L"20300105-000000")));
    CHECK(BackupHostSaves(paths, real, 104729, "20261119-130500", 5, br, error) && IsDir(br.dir) && br.pruned == 1);
    CHECK(IsDir(JoinPath2(HostBackupsDir(paths), L"20261119-130000")) && !IsDir(JoinPath2(HostBackupsDir(paths), L"20300102-000000")));
    CHECK(SaveTreeManifest(real) == real_before);   // only read

    // ---- The real model: Host / Join with the default (host_save on) and with the Debug option off ----
    auto env = std::make_shared<FakeEnv>();
    env->steam = true;
    {
        auto m = MakeRealModelWithEnv(dir, env);
        CHECK(m->phase() == Phase::ready && m->debug_options().host_save && m->debug_options().backup_host_saves);
        RemoveTree(HostBackupsDir(paths));
        std::string code;
        CHECK(m->host(code, error));
        SessionIni ini;
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error));
        CHECK(ini.role == "host" && ini.host_save && ini.sandbox_saves.empty());   // the host plays on its own saves
        // the online session's features: nothing turns the shared vehicles, riding or hits off
        CHECK(text.find("host_save=1\n") != std::string::npos && text.find("partner_body=real\n") != std::string::npos);
        for (const char* off : {"vehicle_sync", "ride_together", "partner_vehicle_damage=0"}) CHECK(text.find(off) == std::string::npos);
        int backups = 0;
        for (const DirEntry& e : ListDirectory(HostBackupsDir(paths))) backups += e.directory ? 1 : 0;
        CHECK(backups == 1);
        // The game runs and exits: the non-slot files are compared (launcher.log).
        env->game_running = true;
        env->game.pid = 9;
        env->game.path = paths.exe;
        m->tick();
        WriteFile(JoinPath2(acct, L"autosave3\\checkpoint.dat"), "the host's game saved");   // a slot: not reported
        env->game_running = false;
        m->tick();
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::after_game);
        CHECK(ReadTextShared(paths.launcher_log, text) && text.find("host_save=checked nonslot_changed=0") != std::string::npos &&
              text.find("host_save=host backup=ok") != std::string::npos);
        env->now += 5000;
        m->tick();
        DeleteFileChecked(JoinPath2(acct, L"autosave3\\checkpoint.dat"));
        RemoveTree(JoinPath2(acct, L"autosave3"));

        // Join: the guest's game runs only on coop-guest (seeded, its keys together in session.ini).
        WriteFile(JoinPath2(gacct, L"manualsave6\\checkpoint.dat"), "last session");
        const std::string partner = FormatJoinCode("7QK2MX9RT4PB8WZ3HCNA", kSteamIdBase + 7919ull);
        CHECK(m->join(partner, error));
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error));
        CHECK(ini.role == "guest" && ini.host_save && ini.sandbox_saves == "coop-guest" && ini.sandbox_foreign_saves && !ini.second_instance);
        CHECK(text.find("host_save=1\n") != std::string::npos && text.find("partner_body=real\n") != std::string::npos);
        for (const char* off : {"vehicle_sync", "ride_together", "partner_vehicle_damage=0"}) CHECK(text.find(off) == std::string::npos);
        CHECK(!IsDir(JoinPath2(gacct, L"manualsave6")) && SandboxSeeded(cg));   // the last session's copy wiped (D1-9)
        // A refusal by the mod (no redirect) ends the session and says so.
        WriteFile(JoinPath2(paths.mod, L"sandbox-refused.txt"), "sandbox=REFUSED\nreason=role=guest with host_save=1 but no save redirect\n");
        m->tick();
        CHECK(m->phase() == Phase::ready && m->test_save().tone == TestSaveView::Tone::bad &&
              m->test_save().result.find("host_save") != std::string::npos);
        DeleteFileChecked(JoinPath2(paths.mod, L"sandbox-refused.txt"));
        // A normal guest game: confirmed redirect, the real tree checked after it.
        CHECK(m->join(partner, error));
        env->game_running = true;
        m->tick();
        WriteFile(JoinPath2(paths.mod, L"sandbox-active.txt"), "sandbox=redirected\n");
        env->now += 25000;
        m->tick();
        env->game_running = false;
        m->tick();
        env->now += 5000;
        m->tick();
        CHECK(m->phase() == Phase::after_game && m->test_save().tone == TestSaveView::Tone::good);
        env->now += 5000;
        m->tick();

        // The Debug option off: the old mode (each on their own save), no backup, no coop-guest keys.
        DebugOptions o = m->debug_options();
        o.host_save = false;
        m->set_debug_options(o);
        RemoveTree(HostBackupsDir(paths));
        CHECK(m->host(code, error));
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && !ini.host_save &&
              text.find("host_save") == std::string::npos);
        CHECK(!IsDir(HostBackupsDir(paths)));
        CHECK(m->cancel_launch(error));
        CHECK(m->join(partner, error));
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && !ini.host_save && ini.sandbox_saves.empty());
        CHECK(m->cancel_launch(error));
        // Backups off, host_save on: no backup.
        o.host_save = true;
        o.backup_host_saves = false;
        m->set_debug_options(o);
        CHECK(m->host(code, error) && !IsDir(HostBackupsDir(paths)));
        CHECK(m->cancel_launch(error));
    }
    {
        // Remembered (launcher-settings.json).
        auto m = MakeRealModelWithEnv(dir, env);
        CHECK(m->debug_options().host_save && !m->debug_options().backup_host_saves);
    }
    // --write-session local-test --out: the local test's two files from this folder's settings (read only), written only
    // into --out (the testenv's -LauncherIni runs play exactly these).
    {
        const std::wstring out = NewTempDir(L"st-launcher-ltini-");
        const std::string mod_before = SaveTreeManifest(paths.mod);
        std::string report;
        CHECK(RunWriteLocalTestIni(dir, JoinPath2(out, L"launcher-ini"), report) == 0 && report.find("result=ok") != std::string::npos);
        CHECK(report.find("host_save=1") != std::string::npos);
        SessionIni hi, gi;
        CHECK(ReadTextShared(JoinPath2(out, L"launcher-ini\\host.ini"), text) && ParseSessionIniText(text, hi, error) && hi.role == "host" &&
              hi.host_save && hi.sandbox_saves == "local-host");
        CHECK(ReadTextShared(JoinPath2(out, L"launcher-ini\\guest.ini"), text) && ParseSessionIniText(text, gi, error) && gi.role == "guest" &&
              gi.host_save && gi.sandbox_saves == "local-guest" && gi.secret == hi.secret);
        CHECK(SaveTreeManifest(paths.mod) == mod_before);   // nothing written in the game folder
        CHECK(RunWriteLocalTestIni(dir, L"", report) != 0);
        // A folder without launcher settings: the launcher's defaults
        CHECK(RunWriteLocalTestIni(out, JoinPath2(out, L"defaults"), report) == 0 &&
              ReadTextShared(JoinPath2(out, L"defaults\\guest.ini"), text) && text.find("host_save=1\n") != std::string::npos);
        RemoveTree(out);
    }
    // Without a Steam account the guest's folder cannot be made: Join refuses (never the real saves).
    {
        auto nosteam = std::make_shared<FakeEnv>();
        auto m = MakeRealModelWithEnv(dir, nosteam);
        const std::string partner = FormatJoinCode("7QK2MX9RT4PB8WZ3HCNA", kSteamIdBase + 7919ull);
        CHECK(!m->join(partner, error) && error.find("Steam") != std::string::npos && !IsFile(paths.session_ini));
    }
    CHECK(SaveTreeManifest(real) == real_before);   // after everything: the real tree untouched

    SetEnvironmentVariableW(L"LOCALAPPDATA", had ? old_local : nullptr);
    RemoveTree(dir);
    RemoveTree(local);
}

// Runs `f` with %LOCALAPPDATA% on an empty temporary folder: the real model's Host and Join (backups, the guest's
// folder) never read the user's own save tree in a test.
void WithFakeSaveRoot(void (*f)()) {
    const std::wstring local = NewTempDir(L"st-launcher-noroot-");
    wchar_t old_local[1024]{};
    const DWORD had = GetEnvironmentVariableW(L"LOCALAPPDATA", old_local, 1024);
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    f();
    SetEnvironmentVariableW(L"LOCALAPPDATA", had ? old_local : nullptr);
    RemoveTree(local);
}

// Fragile's textures (docs/textures.md): session.ini textures_dir (written for every session and both Local test games,
// parsed strictly), the child's progress lines, the command line, the folder rules and the automation guard, the mock's
// card states and the command-line options.
void TexturesSettings() {
    SessionIni ini;
    ini.role = "guest";
    ini.secret = "K7RD3WQ9MP2XVB6TZHN4";
    ini.peer_steamid = "76561197960370457";
    ini.textures_dir = "C:\\Users\\player\\Documents\\Stranded Together\\Textures";
    std::string text, error;
    CHECK(MakeSessionIniText(ini, text, error));
    CHECK(text.find("\ntextures_dir=C:\\Users\\player\\Documents\\Stranded Together\\Textures\n") != std::string::npos);
    SessionIni back;
    CHECK(ParseSessionIniText(text, back, error) && back.textures_dir == ini.textures_dir);
    ini.textures_dir = "relative\\folder";
    CHECK(!MakeSessionIniText(ini, text, error));
    ini.textures_dir = "C:\\a\nrole=host";
    CHECK(!MakeSessionIniText(ini, text, error));
    ini.textures_dir.clear();
    CHECK(MakeSessionIniText(ini, text, error) && text.find("textures_dir") == std::string::npos);
    CHECK(!ParseSessionIniText("# Generated by Stranded Together\nrole=guest\ntextures_dir=nope\n", back, error));
    // Both Local test games get the folder (only the guest's Fragile uses it).
    LocalTestChoices c;
    c.textures_dir = "D:\\My textures";
    SessionIni h, g;
    MakeLocalTestInis(DebugOptions{}, c, h, g);
    CHECK(h.textures_dir == "D:\\My textures" && g.textures_dir == "D:\\My textures");
    // Settings: the default folder unless one was picked; previews on by default.
    json::Value s = json::Value::Object();
    const std::wstring def = DefaultTexturesDir();
    CHECK(TexturesDirFromSettings(s) == def && TexturePreviewsFromSettings(s));
    CHECK(def.empty() || def.find(L"\\Stranded Together\\Textures") != std::wstring::npos);
    json::Value t = json::Value::Object();
    t["dir"] = "E:\\Mods\\Fragile";
    t["previews"] = false;
    s["textures"] = t;
    CHECK(TexturesDirFromSettings(s) == L"E:\\Mods\\Fragile" && !TexturePreviewsFromSettings(s));
    // Her boots (round 6 T3): "own" unless "prop" was picked; written for both games and parsed strictly.
    CHECK(BootsLookFromSettings(s) == "own");
    t["boots"] = "prop";
    s["textures"] = t;
    CHECK(BootsLookFromSettings(s) == "prop");
    t["boots"] = "chunky";
    s["textures"] = t;
    CHECK(BootsLookFromSettings(s) == "own");
    {
        LocalTestChoices bc;
        bc.boots_look = "prop";
        SessionIni bh, bg;
        MakeLocalTestInis(DebugOptions{}, bc, bh, bg);
        CHECK(bh.boots_look == "prop" && bg.boots_look == "prop");
        std::string bt, be;
        CHECK(MakeSessionIniText(bg, bt, be) && bt.find("\nboots_look=prop\n") != std::string::npos);
        SessionIni bb;
        CHECK(ParseSessionIniText(bt, bb, be) && bb.boots_look == "prop");
        bg.boots_look = "chunky";
        CHECK(!MakeSessionIniText(bg, bt, be));
        CHECK(!ParseSessionIniText("# Generated by Stranded Together\nrole=guest\nboots_look=chunky\n", bb, be));
        bg.boots_look.clear();
        CHECK(MakeSessionIniText(bg, bt, be) && bt.find("boots_look") == std::string::npos);
    }
    // Folder rules.
    CHECK(TexturesFolderProblem(L"E:\\Mods\\Fragile", L"C:\\Games\\DS").empty());
    CHECK(!TexturesFolderProblem(L"C:\\Games\\DS\\textures", L"C:\\Games\\DS").empty());
    CHECK(!TexturesFolderProblem(L"textures", L"C:\\Games\\DS").empty());
    CHECK(!TexturesFolderProblem(L"\\\\server\\share\\x", L"C:\\Games\\DS").empty());
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    CHECK(!TexturesFolderProblem(std::wstring(win) + L"\\Textures", L"C:\\Games\\DS").empty());
    // session.ini gets the folder only when the mod can use it: a network (redirected) folder never stops a session.
    {
        json::Value ss = json::Value::Object();
        json::Value tt = json::Value::Object();
        tt["dir"] = "\\\\server\\home\\player\\Documents\\Stranded Together\\Textures";
        ss["textures"] = tt;
        std::string why;
        CHECK(TexturesDirForSession(ss, L"C:\\Games\\DS", &why).empty() && !why.empty());
        CHECK(!TexturesFolderNote(L"\\\\server\\home\\x", true, L"C:\\Games\\DS").empty());
        tt["dir"] = "E:\\Mods\\Fragile";
        ss["textures"] = tt;
        CHECK(TexturesDirForSession(ss, L"C:\\Games\\DS") == "E:\\Mods\\Fragile");
        tt["dir"] = "C:\\Games\\DS\\textures";
        ss["textures"] = tt;
        CHECK(TexturesDirForSession(ss, L"C:\\Games\\DS").empty());
        // the default is never a network path or inside OneDrive (it moves to LocalAppData then)
        if (!def.empty()) CHECK(TexturesFolderProblem(def, L"C:\\Games\\DS").empty() && !InOneDrive(def));
    }
    // OneDrive: a folder under the OneDrive root counts as synced.
    {
        wchar_t prev[MAX_PATH + 1] = {};
        const DWORD had = GetEnvironmentVariableW(L"OneDrive", prev, MAX_PATH + 1);
        SetEnvironmentVariableW(L"OneDrive", L"Q:\\OneDriveTest");
        CHECK(InOneDrive(L"Q:\\OneDriveTest\\Documents\\Stranded Together\\Textures") && !InOneDrive(L"Q:\\Other\\Textures"));
        CHECK(TexturesFolderNote(L"Q:\\OneDriveTest\\Textures", true, L"C:\\Games\\DS").find("OneDrive") != std::string::npos);
        SetEnvironmentVariableW(L"OneDrive", had ? prev : nullptr);
    }
    // The automation guard: never the user's Documents / Desktop / profile, the temp folder is fine.
    wchar_t tmp[MAX_PATH + 1] = {};
    GetTempPathW(MAX_PATH + 1, tmp);
    CHECK(AutomationOutProblem(std::wstring(tmp) + L"st-test\\textures").empty());
    if (!def.empty()) CHECK(!AutomationOutProblem(def).empty());
    // The child's lines.
    ExtractProgress p;
    ApplyExtractLine("progress 3 34 Fragile porter suit\\Original\\Boots\r", p);
    CHECK(p.done == 3 && p.total == 34 && p.current == "Fragile porter suit\\Original\\Boots");
    ApplyExtractLine("set fgl_body_suiti_v00_set: 6 maps, 14.4 MB", p);
    ApplyExtractLine("skip Sam porter suit\\16 Gold\\Hood (sea otter) - Normal: BC6U is not decoded", p);
    ApplyExtractLine("kept Fragile porter suit\\16 Gold\\Suit - Colour.png", p);
    ApplyExtractLine("progress 99 34 nonsense", p);   // out of range: ignored
    CHECK(p.sets == 1 && p.skipped == 1 && p.kept == 1 && p.done == 3 && !p.finished);
    ApplyExtractLine("done 284 80 285177111 7423", p);
    CHECK(p.finished && p.written == 284 && p.kept == 80 && p.bytes == 285177111ull && p.ms == 7423 && p.done == 34);
    CHECK(!p.defaults_seen);
    ApplyExtractLine("defaults 3 20 1 1520", p);
    CHECK(p.defaults_seen && p.defaults_written == 3 && p.defaults_replaced == 20 && p.folders_yours == 1 && p.defaults_ms == 1520);
    ApplyExtractLine("error Not enough free disk space", p);
    CHECK(p.error == "Not enough free disk space");
    // round 9 review: every note counts (a later one never hides "not made"), and the pictures left for the next run
    CHECK(p.note.empty() && !p.defaults_not_made && p.incomplete == 0);
    ApplyExtractLine("note ready-made colours not made: her Original pictures do not read (x) (the colour folders hold copies of her own pictures)", p);
    ApplyExtractLine("note 2 unedited seeds are from an older game version: delete them to refresh", p);
    CHECK(p.defaults_not_made && p.note.find("ready-made colours not made") == 0 && p.note.find(". 2 unedited seeds") != std::string::npos);
    ApplyExtractLine("incomplete 3 2", p);
    CHECK(p.incomplete == 3);
    CHECK(FormatMegabytes(285177111ull) == "272 MB");
    const std::wstring cmd = ExtractCommandLine(L"C:\\G\\StrandedTogether.exe", L"C:\\G\\", L"D:\\T", false);
    CHECK(cmd == L"\"C:\\G\\StrandedTogether.exe\" --extract-textures --game-dir \"C:\\G\" --out \"D:\\T\" --no-previews");
    // The command line it builds parses back.
    {
        const st::ui::Options o = st::ui::ParseCommandLine({L"--extract-textures", L"--game-dir", L"C:\\G", L"--out", L"D:\\T", L"--no-previews"});
        CHECK(o.error.empty() && o.mode == st::ui::Options::Mode::extract_textures && o.write_out == L"D:\\T" && !o.textures_previews);
        CHECK(!st::ui::ParseCommandLine({L"--extract-textures"}).error.empty());   // --out needed
        CHECK(!st::ui::ParseCommandLine({L"--extract-textures", L"--out", L"D:\\T", L"--code", L"x"}).error.empty());
        CHECK(!st::ui::ParseCommandLine({L"--no-previews"}).error.empty());
        CHECK(!st::ui::ParseCommandLine({L"--extract-textures", L"--selftest", L"--out", L"D:\\T"}).error.empty());
        const st::ui::Options k = st::ui::ParseCommandLine({L"--check-textures", L"--out", L"D:\\T"});
        CHECK(k.error.empty() && k.mode == st::ui::Options::Mode::check_textures);
        const st::ui::Options a = st::ui::ParseCommandLine({L"--extract-textures", L"--out", L"D:\\T", L"--allow-out-under", L"D:\\", L"--cancel-after-sets", L"10"});
        CHECK(a.error.empty() && a.textures_allow_under == L"D:\\" && a.textures_cancel_after == 10);
    }
    // The refusals of the --extract-textures entry itself (nothing is written).
    {
        std::vector<std::string> lines;
        auto print = [&](const std::string& l) { lines.push_back(l); };
        ExtractRequest r;
        r.out_dir = L"relative";
        CHECK(RunExtractTextures(r, print) == 1 && !lines.empty() && lines.back().rfind("error ", 0) == 0);
        r.out_dir = std::wstring(tmp) + L"st-test-none\\out";
        r.game_dir = std::wstring(tmp) + L"st-test-none\\no-game";
        CHECK(RunExtractTextures(r, print) == 1 && lines.back().find("ds.exe") != std::string::npos);
        CHECK(GetFileAttributesW(r.out_dir.c_str()) == INVALID_FILE_ATTRIBUTES);
    }
    // The card's child process and pipe (core::TextureTasks) with the real StrandedTogether.exe next to this test: a game
    // folder without ds.exe -> the child's "error ..." line arrives through the pipe and its exit code 1 is taken once.
    {
        const std::wstring exe = SelfExePath().substr(0, SelfExePath().find_last_of(L'\\') + 1) + L"StrandedTogether.exe";
        if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            TextureTasks tasks;
            std::string err;
            CHECK(tasks.StartExtract(exe, std::wstring(tmp) + L"st-test-none\\no-game", std::wstring(tmp) + L"st-test-none\\out", true, err));
            CHECK(!tasks.StartExtract(exe, L"C:\\x", L"C:\\y", true, err) || !tasks.extracting());   // one at a time
            ExtractProgress done;
            int code = -1;
            for (int i = 0; i < 200 && !tasks.TakeFinished(done, code); ++i) Sleep(50);
            CHECK(code == 1 && done.error.find("ds.exe") != std::string::npos && !done.finished);
            CHECK(!tasks.TakeFinished(done, code));   // once
        } else {
            std::printf("note: no StrandedTogether.exe next to the test; the child-process check is skipped\n");
        }
    }
    // The mock's card: not extracted -> extracting -> extracted with edits; Check my edits fills the report.
    st::MockOptions mo;
    st::MockModel m(mo);
    m.use_manual_clock(100.0);
    CHECK(m.textures().state == st::TexturesView::State::not_extracted);
    std::string err;
    CHECK(!m.check_textures(err));
    CHECK(m.extract_textures(err) && m.textures().state == st::TexturesView::State::extracting);
    CHECK(!m.extract_textures(err));
    m.advance(10.0);
    m.tick();
    const st::TexturesView v = m.textures();
    CHECK(v.state == st::TexturesView::State::extracted && v.tone == st::TexturesView::Tone::good && !v.edited.empty());
    CHECK(m.check_textures(err) && m.textures().report.find("Edited:") != std::string::npos);
}

// Round 9: the automatic extraction's decision (core_textures.hpp DecideAutoExtract), its persisted failure record, the
// manifest facts, READY-MADE COLOURS in session.ini.
void AutoExtractRules() {
    using st::tex::kExtractVersion;
    AutoExtractFacts f;
    f.folder_usable = true;
    f.now_s = 1763208000;
    // never extracted: start, whatever the folder holds (the extractor refuses a folder of someone else's files itself)
    AutoExtractDecision d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::start && d.reason == "never extracted" && d.user_reason == "not extracted yet");
    // round 9 review: a folder that held an extraction and lost it (deleted to free space, a missing drive) is the player's
    // call; so is too little free space for an automatic run (looked at only when it would start)
    {
        AutoExtractFacts g = f;
        g.was_extracted = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::gone && !AutoExtractWillRun(AutoExtract::gone));
        g.enabled = false;
        CHECK(DecideAutoExtract(g).what == AutoExtract::off);
        g = f, g.low_disk = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::low_disk && !AutoExtractWillRun(AutoExtract::low_disk));
        g.game_running = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::low_disk);
    }
    CHECK(AutoExtractWillRun(AutoExtract::start) && AutoExtractWillRun(AutoExtract::busy) && AutoExtractWillRun(AutoExtract::wait_game) &&
          AutoExtractWillRun(AutoExtract::retry_later) && !AutoExtractWillRun(AutoExtract::not_needed) && !AutoExtractWillRun(AutoExtract::off) &&
          !AutoExtractWillRun(AutoExtract::onedrive) && !AutoExtractWillRun(AutoExtract::cancelled) && !AutoExtractWillRun(AutoExtract::gave_up) &&
          !AutoExtractWillRun(AutoExtract::unreadable) && !AutoExtractWillRun(AutoExtract::no_folder));
    // this version's (or a newer one's) extraction: nothing to do, whatever else is true; painted folders do not matter
    // (the decision reads only the manifest: a painted picture never makes the launcher extract again)
    f.manifest = ManifestFacts{true, true, kExtractVersion, true};
    CHECK(DecideAutoExtract(f).what == AutoExtract::not_needed && DecideAutoExtract(f).reason.empty());
    f.manifest.extract = kExtractVersion + 1;
    CHECK(DecideAutoExtract(f).what == AutoExtract::not_needed);
    {
        AutoExtractFacts g = f;
        g.enabled = false, g.game_running = true, g.failures = 5, g.cancelled = true;
        g.manifest.extract = kExtractVersion;
        CHECK(DecideAutoExtract(g).what == AutoExtract::not_needed);
    }
    // an older version (round 8's extract 2), another slot table: start, with the reason (the card's words without numbers)
    f.manifest.extract = kExtractVersion - 1;
    d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::start && d.reason.find("older Stranded Together (extract " + std::to_string(kExtractVersion - 1) + ", now " +
                                                        std::to_string(kExtractVersion) + ")") != std::string::npos);
    CHECK(d.user_reason == "Stranded Together was updated");
    f.was_extracted = true;   // an older extraction is still one: updated by itself
    CHECK(DecideAutoExtract(f).what == AutoExtract::start);
    f.was_extracted = false;
    f.manifest = ManifestFacts{true, true, kExtractVersion, false};
    CHECK(DecideAutoExtract(f).what == AutoExtract::start && DecideAutoExtract(f).reason.find("texture table") != std::string::npos);
    // round 9 review: an index that does not read is never extracted over by itself (that extraction would have no record
    // of the unpainted pictures, and a newer Stranded Together's index must not be replaced by an older one); one another
    // program holds for a moment is looked at again
    f.manifest = ManifestFacts{true, false, 0, false};
    d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::unreadable && d.reason.find("does not read") != std::string::npos && !AutoExtractWillRun(d.what));
    f.manifest.open_failed = true;
    d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::look_again && d.reason.find("cannot be opened right now") != std::string::npos && AutoExtractWillRun(d.what));
    {
        AutoExtractFacts g = f;
        g.enabled = false;
        CHECK(DecideAutoExtract(g).what == AutoExtract::off);
    }
    f.manifest = ManifestFacts{true, true, 1, true};   // v0.24
    // the setting off; no usable folder; OneDrive; busy; cancelled by the player
    {
        AutoExtractFacts g = f;
        g.enabled = false;
        CHECK(DecideAutoExtract(g).what == AutoExtract::off);
        g = f, g.folder_usable = false;
        CHECK(DecideAutoExtract(g).what == AutoExtract::no_folder);
        g = f, g.onedrive = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::onedrive);
        g = f, g.extracting = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::busy);
        g = f, g.cancelled = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::cancelled);
        // a game runs (or a session is being started): after it closes, never under it
        g = f, g.game_running = true;
        CHECK(DecideAutoExtract(g).what == AutoExtract::wait_game);
    }
    // failures: 10 minutes after the 1st, 1 hour after the 2nd, none after the 3rd (for that key)
    CHECK(AutoExtractRetryAfterS(0) == 0 && AutoExtractRetryAfterS(1) == 600 && AutoExtractRetryAfterS(2) == 3600 && AutoExtractRetryAfterS(3) == -1 &&
          kAutoExtractMaxFailures == 3);
    f.failures = 1, f.last_failure_s = f.now_s - 300;
    d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::retry_later && d.retry_at_s == f.now_s + 300);
    f.last_failure_s = f.now_s - 601;
    CHECK(DecideAutoExtract(f).what == AutoExtract::start);
    f.failures = 2, f.last_failure_s = f.now_s - 1800;
    d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::retry_later && d.retry_at_s == f.now_s + 1800);
    f.last_failure_s = f.now_s - 3601;
    CHECK(DecideAutoExtract(f).what == AutoExtract::start);
    f.failures = 3, f.last_failure_s = f.now_s - 86400 * 30;
    CHECK(DecideAutoExtract(f).what == AutoExtract::gave_up);
    // a clock that went backwards never blocks it for longer than the wait itself
    f.failures = 1, f.last_failure_s = f.now_s + 86400;
    d = DecideAutoExtract(f);
    CHECK(d.what == AutoExtract::retry_later && d.retry_at_s == f.now_s + 600);
    // waiting for a retry comes before the game check (no process listing needed); a retry due under a game waits for it
    f.last_failure_s = f.now_s - 700, f.game_running = true;
    CHECK(DecideAutoExtract(f).what == AutoExtract::wait_game);

    // the record in launcher-settings.json: kept with the other "textures" fields, removed by an empty key
    json::Value s = json::Value::Object();
    json::Value t = json::Value::Object();
    t["dir"] = "E:\\Mods\\Fragile";
    t["boots"] = "prop";
    s["textures"] = t;
    CHECK(AutoExtractRecordFromSettings(s).key.empty() && AutoExtractFromSettings(s) && ReadyMadeFromSettings(s));
    const std::string key = AutoExtractKey(L"E:\\Mods\\Fragile", "123-456");
    // this build's version first (a Stranded Together update starts from zero), then the extract version, the game, the folder
    const std::string tail = std::to_string(kExtractVersion) + "|123-456|e:\\mods\\fragile";
    CHECK(key.size() > tail.size() && key.compare(key.size() - tail.size(), tail.size(), tail) == 0 && key[key.size() - tail.size() - 1] == '|');
    CHECK(key == AutoExtractKey(L"e:\\MODS\\fragile", "123-456"));
    // the folder last seen extracted: by its lower-case path, kept with the other fields, never twice
    CHECK(!WasExtractedHere(s, L"E:\\Mods\\Fragile") && RememberExtractedHere(s, L"E:\\Mods\\Fragile\\"));
    CHECK(WasExtractedHere(s, L"e:\\mods\\FRAGILE") && !WasExtractedHere(s, L"E:\\Mods\\Other") && !RememberExtractedHere(s, L"e:\\mods\\fragile"));
    CHECK(TexturesDirFromSettings(s) == L"E:\\Mods\\Fragile" && BootsLookFromSettings(s) == "prop");
    // a folder that does not exist yet: its drive's free space (never "unknown")
    CHECK(FreeBytesFor(L"C:\\") > 0 && FreeBytesFor(L"C:\\") != UINT64_MAX && FreeBytesFor(L"C:\\st-no-such-folder\\deeper") != UINT64_MAX);
    StoreAutoExtractRecord(s, AutoExtractRecord{key, 2, 1763208000, "No oo2core"});
    std::string text = json::Dump(s);
    json::Value back;
    CHECK(json::Parse(text, back));
    AutoExtractRecord r = AutoExtractRecordFromSettings(back);
    CHECK(r.key == key && r.failures == 2 && r.last_s == 1763208000 && r.error == "No oo2core");
    CHECK(TexturesDirFromSettings(back) == L"E:\\Mods\\Fragile" && BootsLookFromSettings(back) == "prop");
    StoreAutoExtractRecord(back, AutoExtractRecord{});
    CHECK(AutoExtractRecordFromSettings(back).key.empty() && TexturesDirFromSettings(back) == L"E:\\Mods\\Fragile" && BootsLookFromSettings(back) == "prop");
    t["auto_extract"] = false;
    t["defaults"] = false;
    s["textures"] = t;
    CHECK(!AutoExtractFromSettings(s) && !ReadyMadeFromSettings(s));

    // what the folder's manifest says
    {
        const std::wstring dir = NewTempDir(L"st-autoextract-facts-");
        std::string error;
        ManifestFacts mf = ReadManifestFacts(dir);
        CHECK(!mf.present && !mf.readable);
        CHECK(!ReadManifestFacts(dir + L"\\missing").present && !ReadManifestFacts(L"").present);
        CHECK(WriteAtomic(dir + L"\\.manifest.txt", std::string("not a manifest"), true, error));
        mf = ReadManifestFacts(dir);
        CHECK(mf.present && !mf.readable && !mf.open_failed);
        {   // held by another program (no sharing): there, nothing known, looked at again (never "does not read")
            const HANDLE h = CreateFileW((dir + L"\\.manifest.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            mf = ReadManifestFacts(dir);
            CHECK(h != INVALID_HANDLE_VALUE && mf.present && !mf.readable && mf.open_failed);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
        st::tex::Manifest m;
        m.table = st::tex::kTableVersion;
        m.game = "1-2";
        m.extract = kExtractVersion - 1;
        CHECK(WriteAtomic(dir + L"\\.manifest.txt", st::tex::WriteManifest(m), true, error));
        mf = ReadManifestFacts(dir);
        CHECK(mf.present && mf.readable && mf.table_ok && mf.extract == kExtractVersion - 1);
        m.table ^= 1u;
        m.extract = kExtractVersion;
        CHECK(WriteAtomic(dir + L"\\.manifest.txt", st::tex::WriteManifest(m), true, error));
        mf = ReadManifestFacts(dir);
        CHECK(mf.present && mf.readable && !mf.table_ok && mf.extract == kExtractVersion);
        RemoveTree(dir);
    }

    // READY-MADE COLOURS: only textures_defaults=0 is written (the mod's default is on), for both Local test games
    LocalTestChoices c;
    c.textures_defaults = false;
    SessionIni h, g;
    MakeLocalTestInis(DebugOptions{}, c, h, g);
    CHECK(!h.textures_defaults && !g.textures_defaults);
    std::string it, ie;
    CHECK(MakeSessionIniText(g, it, ie) && it.find("\ntextures_defaults=0\n") != std::string::npos);
    SessionIni ib;
    CHECK(ParseSessionIniText(it, ib, ie) && !ib.textures_defaults);
    g.textures_defaults = true;
    CHECK(MakeSessionIniText(g, it, ie) && it.find("textures_defaults") == std::string::npos);
    CHECK(ParseSessionIniText(it, ib, ie) && ib.textures_defaults);
    CHECK(!ParseSessionIniText("# Generated by Stranded Together\nrole=guest\ntextures_defaults=maybe\n", ib, ie));
    // the command line of an automation child (tests): --allow-out-under at the end
    CHECK(ExtractCommandLine(L"C:\\G\\StrandedTogether.exe", L"C:\\G", L"D:\\T\\x", true, L"D:\\T\\") ==
          L"\"C:\\G\\StrandedTogether.exe\" --extract-textures --game-dir \"C:\\G\" --out \"D:\\T\\x\" --allow-out-under \"D:\\T\"");
}

// Round 9: the real model's automatic extraction on a temporary fake game folder (zero-byte ds.exe, no game data), with
// the real StrandedTogether.exe next to this test as the child: the textures folder is a temporary one named in the fake
// folder's settings (never the player's). The child cannot read a game there, so every run fails: the failure record,
// the waits, the restart rule, the game check, a cancel and the setting are what is checked.
void AutoExtractModel() {
    const std::wstring exe = SelfExePath().substr(0, SelfExePath().find_last_of(L'\\') + 1) + L"StrandedTogether.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("note: no StrandedTogether.exe next to the test; the automatic extraction model check is skipped\n");
        return;
    }
    const std::wstring dir = NewTempDir(L"st-autoextract-game-");
    const std::wstring tex = NewTempDir(L"st-autoextract-tex-") + L"\\Textures";
    std::string error;
    CHECK(WriteAtomic(JoinPath2(dir, L"ds.exe"), std::string(), true, error));
    const InstancePaths paths = GetInstancePaths(dir);
    CHECK(WriteAtomic(paths.mod_dll, FakeOurDll(), true, error));
    CHECK(CreateDirectories(paths.mod));
    {   // the fake folder's settings name the temporary textures folder, automatic extraction off at first
        json::Value s = json::Value::Object();
        s["product"] = "stranded-together-launcher-settings";
        s["v"] = 1;
        json::Value t = json::Value::Object();
        t["dir"] = st::tex::Narrow(tex);
        t["auto_extract"] = false;
        s["textures"] = t;
        CHECK(WriteAtomic(paths.settings, json::Dump(s), true, error));
    }
    {   // never the player's own folder: the model must read exactly this temporary one, or the check stops here
        json::Value s;
        if (!ReadSettings(paths, s) || TexturesDirFromSettings(s) != tex) {
            std::printf("the fake game folder's settings do not name the temporary textures folder: stopped\n");
            CHECK(false);
            RemoveTree(dir);
            return;
        }
    }
    auto env = std::make_shared<FakeEnv>();
    ModelStart start;
    start.auto_extract_textures = true;
    start.extract_exe = exe;
    auto settings = [&]() {
        json::Value s;
        std::string text;
        CHECK(ReadTextShared(paths.settings, text) && json::Parse(text, s));
        return s;
    };
    // ticks until the running extraction ended (the child fails within a second: no game here)
    auto run_out = [&](LauncherModel& m) {
        for (int i = 0; i < 300 && m.textures().state == TexturesView::State::extracting; ++i) {
            Sleep(20);
            m.tick();
        }
        m.tick();
        return m.textures().state != TexturesView::State::extracting;
    };
    // the card once its folder scan is in: the scan is a thread the tick starts (a new model's first one when textures()
    // was asked for, another after each extraction), and until it ends the status is "Looking at the folder..." -- on a
    // loaded machine (ctest -j) still at the next line. Only waits: no tick, so the fake clock's decisions are unchanged.
    auto scanned = [&](LauncherModel& m) {
        TexturesView v = m.textures();
        for (int i = 0; i < 500 && v.state == TexturesView::State::unknown; ++i) {
            Sleep(10);
            v = m.textures();
        }
        return v;
    };
    {
        auto m = MakeRealModelWithEnv(dir, env, start);
        CHECK(m->phase() == Phase::ready);
        // the setting off: nothing happens, the folder is not created
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && !m->textures().auto_extract && m->textures().auto_note.empty());
        CHECK(GetFileAttributesW(tex.c_str()) == INVALID_FILE_ATTRIBUTES);
        // on: the next tick starts it (never extracted), the card shows it as automatic
        m->set_auto_extract(true);
        CHECK(settings().Find("textures")->Bool("auto_extract", false));
        m->tick();
        TexturesView v = m->textures();
        CHECK(v.state == TexturesView::State::extracting && v.auto_running && v.status == "Extracting automatically...");
        CHECK(run_out(*m));
        // failure 1: recorded (persisted), a quiet line, no error result; the next try in 10 minutes
        v = m->textures();
        CHECK(v.result.empty() && v.auto_note.find("tries again in 10 minutes") != std::string::npos);
        AutoExtractRecord r = AutoExtractRecordFromSettings(settings());
        CHECK(r.failures == 1 && r.key == AutoExtractKey(tex, st::tex::GameStamp(dir)) && !r.error.empty());
        env->now += 5 * 60 * 1000;
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.find("5 minutes") != std::string::npos);
        env->now += 6 * 60 * 1000;
        m->tick();
        CHECK(m->textures().state == TexturesView::State::extracting && run_out(*m));
        CHECK(AutoExtractRecordFromSettings(settings()).failures == 2 && m->textures().auto_note.find("60 minutes") != std::string::npos);
    }
    {   // a launcher started again at once does not try again at once (the record is persisted)
        auto m = MakeRealModelWithEnv(dir, env, start);
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.find("tries again in") != std::string::npos);
        // a game of this folder runs when the wait is over: after it closes
        env->now += 61 * 60 * 1000;
        env->game_running = true;
        env->game.pid = 9;
        env->game.path = paths.exe;
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.find("once the game is closed") != std::string::npos &&
              m->textures().auto_note.find("not extracted yet") != std::string::npos && m->textures().auto_note.find("extract 2") == std::string::npos);
        // the status once this model's first folder scan (started by that tick) is in
        const TexturesView card = scanned(*m);
        CHECK(card.auto_will && card.state == TexturesView::State::not_extracted && card.status.find("by itself") != std::string::npos);
        env->game_running = false;
        env->now += 6000;
        m->tick();
        CHECK(m->textures().state == TexturesView::State::extracting && run_out(*m));
        // failure 3: no more automatic tries for this key, said once on the card (no dialog), even days later
        CHECK(AutoExtractRecordFromSettings(settings()).failures == 3 && m->textures().auto_note.find("stopped after 3 tries") != std::string::npos);
        // the card never says "by itself" then (its status, its intro)
        CHECK(!scanned(*m).auto_will && scanned(*m).status == "Not extracted yet.");
        env->now += 7ll * 86400 * 1000;
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.find("stopped after 3 tries") != std::string::npos);
        // Extract textures by hand forgets the record; that run fails too: its error is the card's result, and it counts as
        // the first failure (the automatic one waits 10 minutes instead of trying the same at once)
        CHECK(m->extract_textures(error) && run_out(*m));
        CHECK(AutoExtractRecordFromSettings(settings()).failures == 1 && m->textures().tone == TexturesView::Tone::bad && !m->textures().result.empty());
        // the error once (the card's result), not again in the note
        CHECK(m->textures().auto_note.find("tries again in 10 minutes") != std::string::npos &&
              m->textures().auto_note.find(m->textures().result) == std::string::npos);
        // ... then the automatic one tries again; the player cancels it: not again until the launcher starts again
        env->now += 11 * 60 * 1000;
        m->tick();
        CHECK(m->textures().state == TexturesView::State::extracting && m->textures().auto_running);
        m->cancel_textures();
        CHECK(run_out(*m));
        // one line about it (the result), the note says nothing more, and the card no longer says "by itself"
        CHECK(m->textures().result.find("cancelled") != std::string::npos && m->textures().result.find("launcher starts again") != std::string::npos);
        CHECK(m->textures().auto_note.empty() && !m->textures().auto_will && AutoExtractRecordFromSettings(settings()).failures == 1);
        env->now += 3600ll * 1000;
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting);
        // READY-MADE COLOURS off: persisted, and the next session's session.ini says textures_defaults=0
        CHECK(m->textures().ready_made);
        m->set_ready_made(false);
        CHECK(!m->textures().ready_made && !settings().Find("textures")->Bool("defaults", true));
        env->steam = true;
        m->tick();
        std::string code, text;
        CHECK(m->host(code, error));
        SessionIni ini;
        CHECK(ReadTextShared(paths.session_ini, text) && ParseSessionIniText(text, ini, error) && !ini.textures_defaults);
        CHECK(text.find("\ntextures_defaults=0\n") != std::string::npos);
    }
    {   // the next start after the cancel: it tries again (a session starting meanwhile: it waits for it)
        auto m = MakeRealModelWithEnv(dir, env, start);
        m->tick();
        const TexturesView v = m->textures();
        CHECK(v.state == TexturesView::State::extracting || v.auto_note.find("once the game is closed") != std::string::npos);
        run_out(*m);
    }
    // a run that cannot read the game fails before anything in the textures folder is created or tidied (RunExtract reads
    // the game first): the temporary folder was never even made
    CHECK(GetFileAttributesW(tex.c_str()) == INVALID_FILE_ATTRIBUTES);
    // round 9 review: a folder this launcher saw extracted that loses its index is not filled again by itself, and an
    // index that does not read is never extracted over by itself
    {
        CHECK(CreateDirectories(tex));
        st::tex::Manifest mf;
        mf.table = st::tex::kTableVersion;
        mf.game = st::tex::GameStamp(dir);
        mf.extract = st::tex::kExtractVersion;
        CHECK(WriteAtomic(tex + L"\\.manifest.txt", st::tex::WriteManifest(mf), true, error));
        env->now += 30ll * 86400 * 1000;
        auto m = MakeRealModelWithEnv(dir, env, start);
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.empty());
        CHECK(WasExtractedHere(settings(), tex));
        CHECK(DeleteFileW((tex + L"\\.manifest.txt").c_str()));
        env->now += 60000;
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.find("no longer in this folder") != std::string::npos &&
              !m->textures().auto_will);
        CHECK(WriteAtomic(tex + L"\\.manifest.txt", std::string("table zz"), true, error));
        env->now += 60000;
        m->tick();
        CHECK(m->textures().state != TexturesView::State::extracting && m->textures().auto_note.find("does not read") != std::string::npos &&
              !m->textures().auto_will);
        DeleteFileW((tex + L"\\.manifest.txt").c_str());
        RemoveDirectoryW(tex.c_str());
    }
    RemoveTree(dir);
    RemoveTree(tex.substr(0, tex.find_last_of(L'\\')));
}

// Round 9, measured on the real game (never in ctest): ST_AUTOEXTRACT_GAME=<game folder> and ST_AUTOEXTRACT_UNDER=<a
// scratch folder outside Documents / Desktop / OneDrive>. The launcher's own decision and child (TextureTasks, the
// automation --allow-out-under) fill <scratch>\LocalAppData\Stranded Together\Textures as a first start would, then as
// the start after this update would (round 8's gold / silver, 09 Mustard painted). The game folder is only read. The
// pictures (game data) are deleted at the end unless ST_AUTOEXTRACT_KEEP=1.
void AutoExtractLive() {
    wchar_t game_buf[MAX_PATH] = {}, under_buf[MAX_PATH] = {}, keep_buf[8] = {};
    if (!GetEnvironmentVariableW(L"ST_AUTOEXTRACT_GAME", game_buf, MAX_PATH) || !GetEnvironmentVariableW(L"ST_AUTOEXTRACT_UNDER", under_buf, MAX_PATH)) return;
    const bool keep = GetEnvironmentVariableW(L"ST_AUTOEXTRACT_KEEP", keep_buf, 8) && keep_buf[0] == L'1';
    const std::wstring game = game_buf, under = under_buf;
    const std::wstring here = SelfExePath().substr(0, SelfExePath().find_last_of(L'\\') + 1);
    const std::wstring exe = here + L"StrandedTogether.exe", textool = here + L"st_textool.exe";
    const std::wstring out = under + L"\\LocalAppData\\Stranded Together\\Textures";
    std::printf("live: game %s, textures %s\n", st::tex::Narrow(game).c_str(), st::tex::Narrow(out).c_str());
    // never a folder the automation may not write (it also paints and deletes there): stopped, not only counted
    if (!AutomationOutProblem(out).empty()) {
        std::printf("live: %s is not a test folder: stopped\n", st::tex::Narrow(out).c_str());
        CHECK(false);
        return;
    }
    auto folder_bytes = [](const std::wstring& root, int& files) {
        std::uint64_t total = 0;
        files = 0;
        std::vector<std::wstring> todo{root};
        while (!todo.empty()) {
            const std::wstring d = todo.back();
            todo.pop_back();
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) todo.push_back(d + L"\\" + fd.cFileName);
                else total += (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, ++files;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return total;
    };
    auto facts = [&]() {
        AutoExtractFacts f;
        f.folder_usable = TexturesFolderProblem(out, game).empty();
        f.onedrive = InOneDrive(out);
        f.manifest = ReadManifestFacts(out);
        f.now_s = 1;
        return f;
    };
    auto run = [&](const char* what, ExtractProgress& p) {
        TextureTasks tasks;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(tasks.StartExtract(exe, game, out, true, err, under));
        int code = -1;
        while (!tasks.TakeFinished(p, code)) Sleep(25);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        int files = 0;
        const std::uint64_t bytes = folder_bytes(out, files);
        std::printf("live: %s: exit %d in %.1f s wall (the child's own %.1f s), %d pictures written, %d kept, ready-made %d written / %d replaced / %d "
                    "yours in %llu ms; folder %d files, %.1f MB on disk%s%s\n",
                    what, code, s, static_cast<double>(p.ms) / 1000.0, p.written, p.kept, p.defaults_written, p.defaults_replaced, p.folders_yours,
                    static_cast<unsigned long long>(p.defaults_ms), files, static_cast<double>(bytes) / 1048576.0, p.error.empty() ? "" : "; error ",
                    p.error.c_str());
        CHECK(code == 0 && p.finished);
    };
    // 1. a first start: never extracted
    AutoExtractDecision d = DecideAutoExtract(facts());
    std::printf("live: decision before: %d (%s)\n", static_cast<int>(d.what), d.reason.c_str());
    CHECK(d.what == AutoExtract::start && d.reason == "never extracted");
    ExtractProgress p;
    run("first start", p);
    CHECK(p.defaults_seen && p.defaults_written == 48 && p.folders_yours == 0);
    CHECK(DecideAutoExtract(facts()).what == AutoExtract::not_needed);
    auto file_sha = [](const std::wstring& f) {
        st::tex::Sha256 h{};
        return st::tex::HashFileSha256(f, h) ? st::tex::Hex(h) : std::string();
    };
    const std::wstring suit = out + L"\\Fragile porter suit\\";
    const wchar_t* names[3] = {L"Suit - Colour.png", L"Suit - Reflectance.png", L"Suit - Roughness.png"};
    std::string gold[3], silver[3];
    for (int k = 0; k < 3; ++k) gold[k] = file_sha(suit + L"16 Gold\\" + names[k]), silver[k] = file_sha(suit + L"15 Silver\\" + names[k]);
    // 2. the start after this update on a round-8 folder: generator 1's gold / silver (st_textool gen1), 09 painted
    if (GetFileAttributesW(textool.c_str()) != INVALID_FILE_ATTRIBUTES) {
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + textool + L"\" gen1 --dir \"" + out + L"\"";
        CHECK(CreateProcessW(textool.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CHECK(code == 0 && file_sha(suit + L"16 Gold\\Suit - Colour.png") != gold[0]);
        {   // 09 painted: one byte appended to its Colour (the user's picture)
            HANDLE h = CreateFileW((suit + L"09 Mustard\\Suit - Colour.png").c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            DWORD put = 0;
            WriteFile(h, "x", 1, &put, nullptr);
            CloseHandle(h);
        }
        const std::string painted = file_sha(suit + L"09 Mustard\\Suit - Colour.png");
        d = DecideAutoExtract(facts());
        std::printf("live: decision after round 8's folder: %d (%s)\n", static_cast<int>(d.what), d.reason.c_str());
        CHECK(d.what == AutoExtract::start && d.reason.find("extract 2, now 3") != std::string::npos);
        run("update from round 8", p);
        CHECK(p.defaults_written == 0 && p.defaults_replaced == 6 && p.folders_yours == 1);
        for (int k = 0; k < 3; ++k)
            CHECK(file_sha(suit + L"16 Gold\\" + names[k]) == gold[k] && file_sha(suit + L"15 Silver\\" + names[k]) == silver[k]);
        CHECK(file_sha(suit + L"09 Mustard\\Suit - Colour.png") == painted);
        CHECK(DecideAutoExtract(facts()).what == AutoExtract::not_needed);
    } else {
        std::printf("live: no st_textool.exe: the update case is skipped\n");
    }
    if (!keep) {
        RemoveTree(under + L"\\LocalAppData");
        std::printf("live: the extracted pictures were deleted\n");
    }
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
    WithFakeSaveRoot(&RealModelFlow);
    TestSaves();
    HostSaveSession();
    LocalTestSettings();
    MouseLockSettings();
    TexturesSettings();
    AutoExtractRules();
    WithFakeSaveRoot(&AutoExtractModel);
    AutoExtractLive();
    std::printf("launcher core tests: %d/%d passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
