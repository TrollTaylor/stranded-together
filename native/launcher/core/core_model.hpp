// Stranded Together launcher core: the real LauncherModel and the windowless
// entries (--watch, --write-session, --end-session, the core self-test).
// A slim rewrite of Diamond Dogs Together's core_model on the same patterns.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core_env.hpp"
#include "core_json.hpp"
#include "core_text.hpp"
#include "core_update.hpp"
#include "launcher_model.hpp"

namespace st::core {

// The update's seams (tests): the HTTP client (null = System32's WinHTTP), the
// restart into the new launcher (null = StartSuccessor of <game>\StrandedTogether.exe)
// and whether update jobs run inline instead of on their own thread.
struct UpdateSeams {
    std::shared_ptr<HttpClient> http;
    std::function<bool(const std::wstring& exe, const std::vector<std::wstring>& args, std::string& error)> relaunch;
    bool synchronous = false;
};

std::unique_ptr<LauncherModel> MakeRealModelWithEnv(const std::wstring& exe_dir, std::shared_ptr<Env> env, const ModelStart& start = {},
                                                    UpdateSeams seams = {});

// The Debug page's options: clamped to what session.ini (and the mod) accept,
// kept in launcher-settings.json under "debug", copied into every session.ini
// (with one_shot=1).
DebugOptions ClampDebugOptions(DebugOptions options);
DebugOptions DebugOptionsFromSettings(const json::Value& settings);
void DebugOptionsToSettings(const DebugOptions& options, json::Value& settings);
void ApplyDebugOptions(const DebugOptions& options, SessionIni& ini);

// launcher-settings.json "character_defaults": 2 since Fragile (porter suit) is the guest's
// character. A stored guest pick ("my_character", "test_save"."guest_character") from before it
// that is empty or "sam" (the old default) reads as kGuestCharacter; any other pick is kept
// (GuestPickOrDefault) and counts only with the Debug option "Allow other characters".
inline constexpr int kCharacterDefaults = 2;
std::string GuestPickFromSettings(const json::Value& settings, const std::string& stored);

// The local two-player test's two session.ini (Debug > Test saves > Local test; docs/launcher.md): one place for the
// launcher (start_local_test) and the testenv (--write-session local-test, Start-TestRun.ps1 -LauncherIni), so a hidden
// run plays exactly what the button writes. The host game (Sam, left, keyboard + mouse) on sandbox local-host, the guest
// game (the guest's character, right, pads) on sandbox local-guest; loopback UDP between them; the Debug page's options.
// With options.host_save (default on) both carry host_save=1: the host-held save session (the guest's waiting screen,
// the save transfer) and on it the shared vehicles, riding together and vehicle hits (docs/coop-session.md). Its guest
// folder is then prepared like coop-guest (SeedSessionGuest(local-guest)); the host plays its test-save copy, never the
// real saves, so no host backup is made.
struct LocalTestChoices {
    std::string name;              // the player name ("" = none)
    std::string guest_character;   // the stored pick (GuestCharacter applies the Debug option)
    bool host_pad = false;         // "Pads on the host too"
    std::string secret;            // "" = a new one
};
void MakeLocalTestInis(const DebugOptions& options, const LocalTestChoices& choices, SessionIni& host, SessionIni& guest);
// Developer / testenv: the two files MakeLocalTestInis + MakeSessionIniText give, written as <out_dir>\host.ini and
// <out_dir>\guest.ini, with the Debug page's options and test-save picks read (read only) from the game folder's
// launcher-settings.json (defaults when there is none). Writes nothing in the game folder.
int RunWriteLocalTestIni(const std::wstring& game_dir, const std::wstring& out_dir, std::string& report);

// --watch <session_id> from the exe's folder.
int RunWatch(const std::wstring& game_dir, const std::string& session_id);

// Developer only: write session.ini exactly as Host / Join / Solo test do,
// without the helper, Steam or the clipboard. `report` gets key=value lines.
struct WriteSessionRequest {
    std::string role;          // host | join | echo | solo
    std::string code;          // join: the host's code
    std::string transport;     // "" (steam) | steam | udp
    std::string udp_peer;
    int udp_port_local = 0;    // 0 = the default
    int udp_port_peer = 0;
};
int RunWriteSession(const std::wstring& game_dir, const WriteSessionRequest& request, std::string& report);
int RunEndSession(const std::wstring& game_dir, std::string& report);

// The core's part of --selftest: pure checks (join code, session.ini, status
// view, Steam library parsing) plus a read-only report on the folder the exe
// runs from. Never writes anything. 0 = the pure checks passed (a folder
// that is not a game folder is reported, not failed).
int RunSelfTest(const std::wstring& exe_dir, std::string& report);

}  // namespace st::core
