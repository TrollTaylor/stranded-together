// Stranded Together: --watch, --write-session, --end-session and --selftest
// (see launcher_entry.hpp).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "launcher_entry.hpp"

#include "core/core_model.hpp"
#include "core/core_textures.hpp"
#include "core/core_update.hpp"
#include "core/core_util.hpp"
#include "launcher_join_code.hpp"
#include "launcher_model_mock.hpp"

namespace st {

namespace {
std::wstring ExeFolder() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring SessionFolder(const std::wstring& game_dir) {
    if (game_dir.empty()) return ExeFolder();
    return core::FullPath(game_dir);
}
}  // namespace

int RunWatch(const std::wstring& session) { return core::RunWatch(ExeFolder(), core::Utf8(session)); }

int RunWriteSession(const ui::Options& o, std::string& report) {
    if (core::Utf8(o.write_role) == "local-test") return core::RunWriteLocalTestIni(SessionFolder(o.game_dir), core::FullPath(o.write_out), report);
    core::WriteSessionRequest r;
    r.role = core::Utf8(o.write_role);
    r.code = core::Utf8(o.write_code);
    r.transport = core::Utf8(o.write_transport);
    r.udp_peer = core::Utf8(o.write_udp_peer);
    r.udp_port_local = o.write_port_local;
    r.udp_port_peer = o.write_port_peer;
    return core::RunWriteSession(SessionFolder(o.game_dir), r, report);
}

int RunEndSession(const std::wstring& game_dir, std::string& report) { return core::RunEndSession(SessionFolder(game_dir), report); }

int RunTextures(const ui::Options& o, const std::function<void(const std::string&)>& print) {
    core::ExtractRequest r;
    r.game_dir = SessionFolder(o.game_dir);
    r.out_dir = core::FullPath(o.write_out);
    if (!o.textures_allow_under.empty()) r.allow_under = core::FullPath(o.textures_allow_under);
    r.previews = o.textures_previews;
    r.cancel_after_sets = o.textures_cancel_after;
    r.check_only = o.mode == ui::Options::Mode::check_textures;
    return core::RunExtractTextures(r, print);
}

void* ClaimFolderInstance(const std::wstring& name, const std::function<bool()>& bring_forward) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS) return mutex;
    // Abandoned by a process that ended, or released a moment ago: take it.
    const DWORD r = WaitForSingleObject(mutex, 1500);
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) return mutex;
    if (bring_forward) bring_forward();
    CloseHandle(mutex);
    return nullptr;
}

void TakeOverAfterUpdate(std::uint32_t pid) {
    core::TakeOverFromPreviousLauncher(core::PreviousLauncherWin32(pid), core::kPreviousLauncherWaitMs);
}

int RunSelfTest(std::string& report) {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* what) {
        ++checks;
        if (!ok) {
            ++failures;
            report += "FAIL: ";
            report += what;
            report += '\n';
        }
    };

    MockModel m(MockOptions{});
    m.use_manual_clock(0.0);
    check(m.phase() == Phase::ready, "mock starts ready");
    std::string code, error;
    check(m.host(code, error) && m.phase() == Phase::launching, "host launches");
    check(CheckJoinCode(code, 0).ok, "the host code is a valid join code");
    check(!m.check_code(code).ok, "the player's own code is refused");
    check(m.join_code() == code, "the session keeps the host code");
    m.advance(4.5);
    m.tick();
    check(m.phase() == Phase::in_game && !m.link().connected, "the game runs, the partner is not there yet");
    m.advance(10.0);
    m.tick();
    const LinkView l = m.link();
    check(l.connected && l.remote == "visible" && l.ping_ms > 0, "the partner connects and appears");
    m.advance(100.0);
    m.tick();
    check(m.phase() == Phase::ready, "the session ends");

    check(!m.join("DST1-NOT-A-CODE", error) && !error.empty(), "a bad code is refused");
    check(m.join(MockModel::PartnerCode(), error) && m.phase() == Phase::launching, "join with a code");
    check(m.cancel_launch(error) && m.phase() == Phase::ready, "cancel while launching");
    check(m.solo_test(error) && m.session_role() == Role::echo, "solo test starts an echo session");
    m.advance(20.0);
    m.tick();
    check(m.link().connected && m.link().headline == "Solo test running", "the echo appears");
    m.advance(100.0);
    m.tick();

    m.set_transport(Transport::udp);
    LanSettings lan;
    m.set_lan(lan);
    check(!m.join(MockModel::PartnerCode(), error) && error.find("LAN") != std::string::npos, "LAN mode needs an address");
    lan.peer_ip = "192.168.1.20";
    m.set_lan(lan);
    check(m.join(MockModel::PartnerCode(), error), "LAN join with an address");
    check(m.session_transport() == Transport::udp, "the session is UDP");
    m.cancel_launch(error);

    std::string path;
    check(m.collect_logs(path, error) && !path.empty(), "collect logs");

    MockOptions offline;
    offline.scenario = MockScenario::steam_offline;
    MockModel o(offline);
    check(!ui::CanHost(o.phase(), o.checks()) && !o.host(code, error), "no hosting without Steam");

    report += "selftest: " + std::to_string(checks - failures) + "/" + std::to_string(checks) + " checks passed\n";
    const int core_result = core::RunSelfTest(ExeFolder(), report);
    return failures == 0 && core_result == 0 ? 0 : 1;
}

}  // namespace st
