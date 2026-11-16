// Stranded Together launcher core: the core part of --selftest (see core_model.hpp).
// Pure checks first; then a read-only report on the folder the exe runs from.
#include "core_model.hpp"

#include "core_install.hpp"
#include "core_paths.hpp"
#include "core_text.hpp"
#include "launcher_join_code.hpp"

#ifndef ST_LAUNCHER_VERSION
#define ST_LAUNCHER_VERSION "0.0.0-dev"
#endif

namespace st::core {

int RunSelfTest(const std::wstring& exe_dir, std::string& report) {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) {
            ++failures;
            report += "FAIL: core: ";
            report += what;
            report += '\n';
        }
    };

    // Join codes (the st_net DST1 parser behind the launcher's wrapper).
    const std::string secret = NewSecret();
    check(IsCanonicalSecret(secret), "a new secret is 20 canonical characters");
    const std::uint64_t host = kSteamIdBase + 123456789ull;
    const std::string code = FormatJoinCode(secret, host);
    check(code.size() == 49 && code.rfind("DST1-", 0) == 0, "a join code is DST1-...@<id>, 49 characters");
    std::string parsed_secret;
    std::uint64_t parsed_host = 0;
    check(ParseJoinCode(code, parsed_secret, parsed_host) && parsed_secret == secret && parsed_host == host,
          "a join code parses back to its secret and host");
    check(ParseJoinCode("+stranded_join " + code, parsed_secret, parsed_host), "a Steam invite string parses");
    check(CheckJoinCode(code, host).ok == false, "the player's own code is refused");
    std::string typo = code;
    typo[6] = typo[6] == 'A' ? 'B' : 'A';
    check(!CheckJoinCode(typo, 0).ok, "a code with a typo is refused");

    // session.ini.
    SessionIni ini;
    ini.role = "guest";
    ini.secret = secret;
    ini.peer_steamid = Decimal(host);
    std::string text, error;
    check(MakeSessionIniText(ini, text, error) && IsGeneratedIniText(text), "a Steam guest session.ini is written");
    SessionIni back;
    check(ParseSessionIniText(text, back, error) && back.role == "guest" && back.secret == secret && back.peer_steamid == ini.peer_steamid &&
              back.transport == "steam",
          "session.ini parses back");
    SessionIni bad = ini;
    bad.peer_steamid.clear();
    check(!MakeSessionIniText(bad, text, error), "a Steam guest without the host's id is refused");
    SessionIni udp;
    udp.role = "host";
    udp.secret = secret;
    udp.transport = "udp";
    udp.udp_peer = "192.168.1.020";
    udp.udp_port_local = udp.udp_port_peer = 27555;
    check(!MakeSessionIniText(udp, text, error), "a non-canonical IPv4 is refused");
    udp.udp_peer = "192.168.1.20";
    check(MakeSessionIniText(udp, text, error) && text.find("udp_peer=192.168.1.20\n") != std::string::npos, "a UDP host session.ini is written");
    check(text.find("one_shot=1\n") != std::string::npos && text.find("debug_menu_key=F9\n") != std::string::npos,
          "session.ini is one-shot and carries the debug menu key");

    // link-status.json.
    const json::Value status = ParseStatusText(
        "{\"v\":1,\"pid\":42,\"state\":\"connected\",\"transport\":\"steam\",\"route\":\"relay\",\"peer\":\"Sam\",\"rtt_ms\":48,"
        "\"remote\":\"visible\",\"reason\":\"\",\"code\":\"\",\"time_utc\":\"2026-11-15T12:00:00Z\"}",
        0);
    check(status.is_object() && status.Int("rtt_ms", 0) == 48, "link-status.json v1 parses");
    check(ParseStatusText("{\"v\":2,\"state\":\"connected\"}", 0).is_null(), "another status version is refused");
    const StatusView view = StatusViewOf(status, true, "host");
    check(view.connected && view.headline == "Connected to Sam", "a connected status reads as connected");
    check(!StatusViewOf(status, false, "host").connected, "a stale status is not connected");
    std::int64_t t = 0;
    ParseUtc("2026-11-15T12:00:10Z", t);
    check(StatusTimeFresh(status, t), "a status 10 s old is fresh");
    ParseUtc("2026-11-15T12:00:20Z", t);
    check(!StatusTimeFresh(status, t), "a status 20 s old is stale");

    // Steam library discovery text.
    const auto libs = SteamLibraryPathsFromVdf("\"libraryfolders\"\n{\n \"0\"\n {\n  \"path\"  \"D:\\\\Games\\\\Steam\"\n }\n}\n");
    check(libs.size() == 1 && libs[0] == L"D:\\Games\\Steam", "libraryfolders.vdf paths are read");
    check(InstallDirFromManifest("\"AppState\"\n{\n\t\"installdir\"\t\t\"DEATH STRANDING DIRECTORS CUT\"\n}\n") ==
              "DEATH STRANDING DIRECTORS CUT",
          "appmanifest installdir is read");

    // Redaction.
    const std::string redacted = Redact("code=" + code + " secret=" + secret);
    check(redacted.find(secret) == std::string::npos && redacted.find(Decimal(host)) == std::string::npos, "logs never carry codes or secrets");

    report += "core: " + std::to_string(checks - failures) + "/" + std::to_string(checks) + " checks passed\n";

    // Read-only report on this folder (never a failure: the exe may run from a build folder).
    const InstancePaths paths = GetInstancePaths(exe_dir);
    if (!IsFile(paths.exe)) {
        report += "game folder: NOT a DEATH STRANDING folder (no ds.exe next to this program: " + Utf8(paths.game) +
                  "). The launcher shows its \"Move me to your game folder\" page here.\n";
        std::shared_ptr<Env> env = MakeWin32Env();
        const auto found = FindGameFolders(*env);
        report += found.empty() ? std::string("steam: no DEATH STRANDING DIRECTOR'S CUT install found\n")
                                : "steam: the game is installed at " + Utf8(found.front()) + "\n";
    } else {
        std::shared_ptr<Env> env = MakeWin32Env();
        json::Value scratch = json::Value::Object();   // the hash cache is not saved: the self-test writes nothing
        const GateResult gate = CheckGate(*env, paths, scratch);
        report += "game folder: " + Utf8(paths.game) + "\n";
        report += std::string("ds.exe: ") + (gate.exe_known ? "tested build" : gate.exe_hashed ? "UNTESTED build (warning only)" : "unreadable") + "\n";
        report += std::string("steam_api64.dll: ") + (gate.steam_api_known ? "tested build" : "untested (warning only)") + "\n";
        const PackageInfo package = ReadPackage(paths);
        const ModCheck mod = CheckMod(paths, package, ST_LAUNCHER_VERSION);
        static const char* const kModStates[] = {"MISSING - co-op cannot start", "ours, matches package.json",
                                                 "ours, development build (no package.json)", "ours, but does NOT match package.json (warning)",
                                                 "NOT OURS - another program's winhttp.dll", "switched off (winhttp.dll.disabled)"};
        report += std::string("mod (winhttp.dll): ") + kModStates[static_cast<int>(mod.state)] + "\n";
        report += "package.json: " + (package.ok ? package.build_id : package.present ? std::string("DAMAGED") : std::string("none")) + "\n";
        report += std::string("steam: ") + (SteamClientRunning(*env) ? "running, logged in" : "not running") + "\n";
        if (!gate.path_problem.empty()) report += "path problem: " + gate.path_problem + "\n";
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace st::core
