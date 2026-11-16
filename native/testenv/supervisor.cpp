// st_testenv_supervisor: runs Death Stranding (or the probe / a fake game) on a
// private, never-displayed Windows desktop and drives it through the mod's
// automation channel. docs/hidden-testing.md.
//
//   st_testenv_supervisor --probe <st_testenv_probe.exe> --run-dir <dir> [--seconds N]
//   st_testenv_supervisor --game-dir <dir> --mod <winhttp.dll> --scenario <file> --run-dir <dir>
//                         [--timeout-s N (<=360)] [--saves <dir>] [--seed-saves <dir>] [--no-steam-check] [--keep-audio] [--no-game-stdout]
//                         [--no-mod] [--no-ui-limits] [--steam-env] [--no-isolation]
//   (bisecting a game that does not start: --no-mod runs the game without the mod's session,
//    overlay and game hooks -- winhttp.dll only redirects the saves -- (scenario with
//    supervisor steps only),
//    --no-ui-limits drops the job's UI restrictions (kill-on-close stays), --steam-env adds
//    the variables Steam sets for a game it launches, --no-isolation keeps the automation
//    channel but not the overlay's automation hooks (virtual pad, foreground, cursor,
//    fullscreen, occlusion))
//   st_testenv_supervisor --restore <run dir>
//
// Safety: CreateDesktopW only (never SwitchDesktop/OpenInputDesktop for writing),
// children get STARTUPINFO.lpDesktop and live in a kill-on-close job with UI
// restrictions (no desktop switching, no display-mode or system-parameter
// changes, no clipboard); the supervisor kills only that job. Every second it
// checks that no window of the job is on the user's input desktop and that the
// input desktop did not change; either ends the run at once. Refuses when
// ds.exe already runs. The game folder is backed up to <run>\backup first and
// restored afterwards (restore.txt). The user's saves are hashed and backed up to
// <run>\saves-backup first; the mod redirects every access to them to a seeded
// scratch copy (confirmed before any scenario step runs, otherwise the game is
// killed); afterwards the real tree is compared by hash and restored from the
// backup if anything differs.
//
// GUI subsystem: never opens a console window; prints to the parent console
// when there is one, and always to <run>\supervisor.log.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <regex>
#include <string>
#include <vector>

#include "automation.hpp"
#include "testenv_core.hpp"

namespace te = st::testenv;

namespace {

// Hard cap on the game's lifetime, final screenshot, quit and kill included.
constexpr unsigned kMaxRunSeconds = 360;
// Part of the cap kept for the end of a run (final screenshot, quit, exit, kill):
// the scenario's steps get at most kMaxRunSeconds - kShutdownReserveSeconds.
constexpr unsigned kShutdownReserveSeconds = 45;

// ------------------------------------------------------------------ output
HANDLE g_log = INVALID_HANDLE_VALUE;
HANDLE g_stdout = nullptr;  // inherited pipe/file (ctest, redirected shells) or the parent's console
ULONGLONG g_t0 = GetTickCount64();

void Out(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    char line[2300];
    const int n = std::snprintf(line, sizeof(line), "[%8.3f] %s\n", static_cast<double>(GetTickCount64() - g_t0) / 1000.0, msg);
    if (n <= 0) return;
    DWORD w = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, line, static_cast<DWORD>(n), &w, nullptr);
    if (g_stdout) WriteFile(g_stdout, line, static_cast<DWORD>(n), &w, nullptr);
}

void OpenLog(const std::wstring& run_dir) {
    te::EnsureDir(run_dir);
    g_log = CreateFileW((run_dir + L"\\supervisor.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
}

// ------------------------------------------------------------------ desktop
std::string ObjectName(HANDLE h) {
    wchar_t name[256] = {};
    DWORD need = 0;
    if (!h || !GetUserObjectInformationW(h, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &need)) return "?";
    return te::Narrow(name);
}

std::string InputDesktopName() {
    HDESK d = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!d) return "unavailable(" + std::to_string(GetLastError()) + ")";
    const std::string n = ObjectName(d);
    CloseDesktop(d);
    return n;
}

struct Owned {
    HDESK desktop = nullptr;
    std::string desktop_name;
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    std::string initial_input;
    bool violation = false;
    std::string violation_text;
    unsigned private_windows = 0;
    std::string private_titles;
};

bool CreatePrivateDesktop(Owned& o) {
    o.initial_input = InputDesktopName();
    char name[96];
    std::snprintf(name, sizeof(name), "StTestEnv_%lu_%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    o.desktop_name = name;
    // Never SwitchDesktop: this desktop is never shown. Nothing here changes the user's.
    o.desktop = CreateDesktopW(te::Widen(name).c_str(), nullptr, nullptr, 0,
                               DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                   DESKTOP_ENUMERATE | DESKTOP_HOOKCONTROL | GENERIC_READ,
                               nullptr);
    if (!o.desktop) {
        Out("REFUSED: CreateDesktopW failed (error %lu)", GetLastError());
        return false;
    }
    Out("private desktop WinSta0\\%s created (input desktop: %s)", name, o.initial_input.c_str());
    return true;
}

bool CreateJob(Owned& o, bool ui_limits = true) {
    o.job = CreateJobObjectW(nullptr, nullptr);
    if (!o.job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    if (!ui_limits) Out("job WITHOUT UI restrictions (--no-ui-limits): kill-on-close only");
    return SetInformationJobObject(o.job, JobObjectExtendedLimitInformation, &lim, sizeof(lim)) &&
           (!ui_limits || SetInformationJobObject(o.job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)));
}

std::vector<DWORD> JobPids(const Owned& o) {
    std::vector<DWORD> out;
    if (!o.job) return out;
    struct {
        JOBOBJECT_BASIC_PROCESS_ID_LIST h;
        ULONG_PTR more[255];
    } list{};
    if (QueryInformationJobObject(o.job, JobObjectBasicProcessIdList, &list, sizeof(list), nullptr))
        for (DWORD i = 0; i < list.h.NumberOfProcessIdsInList && i < 256; ++i)
            out.push_back(static_cast<DWORD>(i == 0 ? list.h.ProcessIdList[0] : list.more[i - 1]));
    if (o.pid && std::find(out.begin(), out.end(), o.pid) == out.end()) out.push_back(o.pid);
    return out;
}

// The safety check: no window of ours on the user's desktop, input desktop unchanged.
bool SafetyCheck(Owned& o) {
    struct Ctx {
        std::vector<DWORD> pids;
        HWND hit = nullptr;
        DWORD hit_pid = 0;
    } ctx{JobPids(o)};
    // This thread's desktop is the one the supervisor was started on (the user's).
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (std::find(c->pids.begin(), c->pids.end(), pid) != c->pids.end()) {
            c->hit = w;
            c->hit_pid = pid;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hit) {
        wchar_t title[128] = {}, cls[128] = {};
        GetWindowTextW(ctx.hit, title, 128);
        GetClassNameW(ctx.hit, cls, 128);
        o.violation = true;
        o.violation_text = "window " + te::Narrow(cls) + " '" + te::Narrow(title) + "' of pid " + std::to_string(ctx.hit_pid) +
                           " is on the user's desktop" + (IsWindowVisible(ctx.hit) ? " (VISIBLE)" : " (hidden)");
        return false;
    }
    // A ds.exe outside our job (a relaunch through Steam would start one on the user's
    // desktop). Not ours to kill: stop our run and report it.
    for (DWORD pid : te::FindProcesses(L"ds.exe")) {
        if (std::find(ctx.pids.begin(), ctx.pids.end(), pid) == ctx.pids.end()) {
            o.violation = true;
            o.violation_text = "a ds.exe outside the run's job appeared (pid " + std::to_string(pid) + "; Steam relaunch?)";
            return false;
        }
    }
    const std::string input = InputDesktopName();
    if (input != o.initial_input && input.rfind("unavailable", 0) != 0) {
        o.violation = true;
        o.violation_text = "the input desktop changed from '" + o.initial_input + "' to '" + input + "'";
        return false;
    }
    // Inventory of the private desktop (for the report).
    struct Inv {
        unsigned n = 0;
        std::string titles;
    } inv;
    EnumDesktopWindows(o.desktop, [](HWND w, LPARAM lp) -> BOOL {
        auto* i = reinterpret_cast<Inv*>(lp);
        if (!IsWindowVisible(w)) return TRUE;
        ++i->n;
        wchar_t title[96] = {};
        GetWindowTextW(w, title, 96);
        if (i->titles.size() < 300) i->titles += (i->titles.empty() ? "" : " | ") + te::Narrow(title);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&inv));
    if (inv.n >= o.private_windows && inv.n > 0) {  // the most seen at once (the game may have exited by the end)
        o.private_windows = inv.n;
        o.private_titles = inv.titles;
    }
    return true;
}

std::wstring SteamPath() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

bool g_full_steam_env = false;  // --steam-env

std::wstring ChildEnvironment(bool steam_app_id) {
    std::vector<std::wstring> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
            const std::wstring v = p;
            if (steam_app_id && (_wcsnicmp(v.c_str(), L"SteamAppId=", 11) == 0 || _wcsnicmp(v.c_str(), L"SteamGameId=", 12) == 0)) continue;
            if (v.rfind(std::wstring(st::automation::saves::kSupervisedEnv) + L"=", 0) == 0) continue;
            vars.push_back(v);
        }
        FreeEnvironmentStringsW(env);
    }
    // The mod refuses automation=1 in a ds.exe that does not carry this (automation.hpp).
    vars.push_back(std::wstring(st::automation::saves::kSupervisedEnv) + L"=" + std::to_wstring(GetCurrentProcessId()));
    if (steam_app_id) {
        // ds.exe calls SteamAPI_RestartAppIfNecessary(1850570) (0x141A1F670, from
        // 0x1418EA074) and exits when it returns true. steam_api64.dll returns false
        // when GetEnvironmentVariableA("SteamAppId") holds a non-zero number (checked
        // before steam_appid.txt), so the game neither relaunches through Steam (which
        // would open it on the user's desktop) nor needs steam_appid.txt.
        vars.push_back(L"SteamAppId=1850570");
        vars.push_back(L"SteamGameId=1850570");
        if (g_full_steam_env) {  // what Steam itself sets for a game it launches
            for (const wchar_t* v : {L"SteamClientLaunch=1", L"SteamEnv=1", L"SteamOverlayGameId=1850570", L"ENABLE_VK_LAYER_VALVE_steam_overlay_1=1"}) {
                bool have = false;
                for (const std::wstring& e : vars) have = have || _wcsnicmp(e.c_str(), v, wcschr(v, L'=') - v + 1) == 0;
                if (!have) vars.push_back(v);
            }
            const std::wstring sp = SteamPath();
            if (!sp.empty()) vars.push_back(L"SteamPath=" + sp);
        }
    }
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::wstring block;
    for (const std::wstring& v : vars) block += v, block += L'\0';
    block += L'\0';
    return block;
}

// stdout_file: ds.exe prints its own log (FUN_141A11EC0: fwrite to stdout, then
// OutputDebugStringA) -- given a file as stdout, the run keeps it (game-stdout.txt).
// Only that one handle is inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST).
bool Launch(Owned& o, const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, bool steam_env,
            const std::wstring& stdout_file = {}) {
    std::wstring desk = L"WinSta0\\" + te::Widen(o.desktop_name);
    STARTUPINFOEXW six{};
    STARTUPINFOW& si = six.StartupInfo;
    si.cb = sizeof(six);
    si.lpDesktop = desk.data();
    si.dwFlags = STARTF_FORCEOFFFEEDBACK;  // no busy cursor on the user's desktop
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    std::wstring env = ChildEnvironment(steam_env);
    HANDLE out = INVALID_HANDLE_VALUE;
    std::vector<unsigned char> attr_buf;
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP;
    if (!stdout_file.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        out = CreateFileW(stdout_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attr_buf.resize(size);
        six.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        if (out != INVALID_HANDLE_VALUE && InitializeProcThreadAttributeList(six.lpAttributeList, 1, 0, &size) &&
            UpdateProcThreadAttribute(six.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof(out), nullptr, nullptr)) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput = nullptr;
            si.hStdOutput = out;
            si.hStdError = out;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        } else {
            Out("game stdout not captured (error %lu)", GetLastError());
            six.lpAttributeList = nullptr;
        }
    }
    const bool inherit = (flags & EXTENDED_STARTUPINFO_PRESENT) != 0;
    si.cb = inherit ? sizeof(six) : sizeof(STARTUPINFOW);
    const BOOL created = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, inherit ? TRUE : FALSE, flags, env.data(), cwd.c_str(),
                                        &si, &pi);
    const DWORD create_error = GetLastError();
    if (six.lpAttributeList) DeleteProcThreadAttributeList(six.lpAttributeList);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);  // the child has its own copy
    if (!created) {
        Out("CreateProcess(%s) failed (error %lu)", te::Narrow(exe).c_str(), create_error);
        return false;
    }
    if (!AssignProcessToJobObject(o.job, pi.hProcess)) {
        Out("AssignProcessToJobObject failed (error %lu): killing the suspended child", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    o.process = pi.hProcess;
    o.pid = pi.dwProcessId;
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    Out("started %s pid %lu on WinSta0\\%s (job: kill on close, UI restricted)", te::Narrow(exe).c_str(), o.pid, o.desktop_name.c_str());
    return true;
}

bool Alive(const Owned& o) { return o.process && WaitForSingleObject(o.process, 0) == WAIT_TIMEOUT; }

void KillOwned(Owned& o, const char* why) {
    if (!o.job) return;
    const std::vector<DWORD> pids = JobPids(o);
    Out("terminating the job (%zu process(es), %s)", pids.size(), why);
    TerminateJobObject(o.job, 1);
    if (o.process) WaitForSingleObject(o.process, 10000);
    // Wait for every job process to be gone (file handles released).
    for (int i = 0; i < 100; ++i) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
        if (!QueryInformationJobObject(o.job, JobObjectBasicAccountingInformation, &acc, sizeof(acc), nullptr) || acc.ActiveProcesses == 0)
            break;
        Sleep(100);
    }
}

void CloseOwned(Owned& o) {
    if (o.process) CloseHandle(o.process), o.process = nullptr;
    if (o.job) CloseHandle(o.job), o.job = nullptr;  // kill on close: nothing of ours survives
    if (o.desktop) CloseDesktop(o.desktop), o.desktop = nullptr;
}

// ------------------------------------------------------------------ args
struct Args {
    std::wstring probe, game_dir, mod, scenario, run_dir, restore, saves, seed;
    bool steam_check = true, mute = true, no_mod = false, ui_limits = true, steam_env = false, isolation = true, game_stdout = true;
    unsigned timeout_s = kMaxRunSeconds, probe_seconds = 6;
};

bool ParseArgs(Args& a, std::string* err) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return *err = "CommandLineToArgvW", false;
    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        const std::wstring k = argv[i];
        const auto val = [&](std::wstring& dst) {
            if (i + 1 >= argc) return ok = false, *err = "missing value for " + te::Narrow(k), void();
            dst = argv[++i];
        };
        if (k == L"--probe") val(a.probe);
        else if (k == L"--game-dir") val(a.game_dir);
        else if (k == L"--mod") val(a.mod);
        else if (k == L"--scenario") val(a.scenario);
        else if (k == L"--run-dir") val(a.run_dir);
        else if (k == L"--restore") val(a.restore);
        else if (k == L"--saves") val(a.saves);
        else if (k == L"--seed-saves") val(a.seed);
        else if (k == L"--no-steam-check") a.steam_check = false;
        else if (k == L"--no-game-stdout") a.game_stdout = false;
        else if (k == L"--keep-audio") a.mute = false;
        else if (k == L"--no-mod") a.no_mod = true;
        else if (k == L"--no-ui-limits") a.ui_limits = false;
        else if (k == L"--steam-env") a.steam_env = true;
        else if (k == L"--no-isolation") a.isolation = false;
        else if (k == L"--timeout-s" || k == L"--seconds") {
            std::wstring v;
            val(v);
            const unsigned n = static_cast<unsigned>(_wtoi(v.c_str()));
            if (k == L"--seconds") a.probe_seconds = std::clamp(n, 2u, 60u);
            else a.timeout_s = std::clamp(n, 30u, kMaxRunSeconds);
        } else {
            ok = false;
            *err = "unknown argument " + te::Narrow(k);
        }
    }
    LocalFree(argv);
    return ok;
}

// ------------------------------------------------------------------ probe mode
int RunProbe(const Args& a) {
    const std::wstring run = te::FullPath(a.run_dir);
    Owned o;
    if (!CreatePrivateDesktop(o) || !CreateJob(o)) {
        Out("REFUSED: desktop/job setup failed");
        CloseOwned(o);
        return 2;
    }
    const std::wstring exe = te::FullPath(a.probe);
    const std::wstring args = L"--out \"" + run + L"\" --seconds " + std::to_wstring(a.probe_seconds);
    int rc = 1;
    if (Launch(o, exe, args, run, false)) {
        const ULONGLONG end = GetTickCount64() + (a.probe_seconds + 60ull) * 1000;
        bool safe = true;
        while (Alive(o) && GetTickCount64() < end) {
            if (!SafetyCheck(o)) {
                Out("SAFETY VIOLATION: %s", o.violation_text.c_str());
                KillOwned(o, "safety violation");
                safe = false;
                break;
            }
            WaitForSingleObject(o.process, 250);
        }
        if (Alive(o)) KillOwned(o, "probe timeout");
        DWORD code = 1;
        GetExitCodeProcess(o.process, &code);
        Out("probe exited with %lu; windows seen on the private desktop: %u (%s)", code, o.private_windows, o.private_titles.c_str());
        std::string json;
        if (te::ReadFileBytes(run + L"\\probe.json", &json)) Out("probe.json: %s", json.c_str());
        rc = !safe ? 3 : (code == 0 ? 0 : 1);
    }
    const std::string summary = std::string("{\"mode\":\"probe\",\"result\":\"") + (rc == 0 ? "pass" : "fail") +
                                "\",\"violation\":" + (o.violation ? "true" : "false") + ",\"violation_text\":\"" +
                                te::JsonEscape(o.violation_text) + "\",\"desktop\":\"" + o.desktop_name + "\",\"input_desktop\":\"" +
                                te::JsonEscape(o.initial_input) + "\",\"input_desktop_after\":\"" + te::JsonEscape(InputDesktopName()) +
                                "\"}\n";
    te::WriteFileAtomic(run + L"\\summary.json", summary);
    CloseOwned(o);
    Out("probe run %s", rc == 0 ? "PASSED" : "FAILED");
    return rc;
}

// ------------------------------------------------------------------ game mode
struct Run {
    Args args;
    std::wstring run, game, mod_dir, auto_in, auto_out, log_path;
    Owned o;
    te::Scenario scenario;
    ULONGLONG deadline = 0;
    unsigned seq = 0;
    bool quit_sent = false;
    std::vector<std::string> results;  // per step, JSON objects
    int failures = 0;
    std::string end_reason;
    std::string error_before;  // sha256 of <game>\error.txt before the run ("" = none)
    double lifetime_s = 0, cpu_s = 0;
    unsigned long exit_code = 0;
};

bool Expired(const Run& r) { return GetTickCount64() >= r.deadline; }

// Waits `ms` while watching the process and the safety rules. False = stop the run.
bool Watch(Run& r, DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        if (!SafetyCheck(r.o)) {
            Out("SAFETY VIOLATION: %s", r.o.violation_text.c_str());
            KillOwned(r.o, "safety violation");
            r.end_reason = "safety violation";
            return false;
        }
        if (!Alive(r.o)) {
            if (r.end_reason.empty()) r.end_reason = "the game exited";
            return false;
        }
        if (Expired(r)) {
            r.end_reason = "run timeout";
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return true;
        WaitForSingleObject(r.o.process, static_cast<DWORD>(std::min<ULONGLONG>(250, end - now)));
    }
}

// Sends one mod command and waits for its .done. Returns ok; detail = the mod's line.
bool ModCommand(Run& r, const std::string& cmd, std::string* detail) {
    char name[32];
    std::snprintf(name, sizeof(name), "%04u", ++r.seq);
    const std::wstring base = te::Widen(name);
    const std::wstring done = r.auto_out + L"\\" + base + L".done";
    if (!te::WriteFileAtomic(r.auto_in + L"\\" + base + L".tmp", cmd + "\n") ||
        !MoveFileExW((r.auto_in + L"\\" + base + L".tmp").c_str(), (r.auto_in + L"\\" + base + L".cmd").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *detail = "could not write the command file";
        return false;
    }
    if (cmd.rfind("quit", 0) == 0) r.quit_sent = true;
    const ULONGLONG end = GetTickCount64() + te::AckTimeoutSeconds(cmd) * 1000ull;
    bool last_look = false;
    while (GetTickCount64() < end || last_look) {
        std::string text;
        if (te::ReadFileBytes(done, &text)) {
            std::vector<std::string> lines;
            std::size_t at = 0;
            while (at < text.size()) {
                std::size_t nl = text.find('\n', at);
                if (nl == std::string::npos) nl = text.size();
                lines.push_back(text.substr(at, nl - at));
                at = nl + 1;
            }
            *detail = lines.size() >= 3 ? lines[2] : text;
            return !lines.empty() && lines[0] == "ok";
        }
        if (last_look) break;
        if (!Watch(r, 100)) {
            // The ack may have been written just before the process ended (quit).
            if (r.end_reason == "the game exited") {
                last_look = true;
                continue;
            }
            *detail = "no answer: " + r.end_reason;
            return false;
        }
    }
    if (last_look) {
        *detail = "no answer: " + r.end_reason;
        return false;
    }
    *detail = "no answer within " + std::to_string(te::AckTimeoutSeconds(cmd)) + " s";
    return false;
}

bool LogMatches(Run& r, const std::regex& re, std::string* line_out) {
    std::string text;
    if (!te::ReadFileBytes(r.log_path, &text)) return false;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, re)) {
            *line_out = line;
            return true;
        }
    }
    return false;
}

void Record(Run& r, const te::Step& s, const char* what, bool ok, const std::string& detail, ULONGLONG t0) {
    const double secs = static_cast<double>(GetTickCount64() - t0) / 1000.0;
    Out("step line %d %s%s -> %s (%.1f s): %s", s.line, s.optional ? "?" : "", what, ok ? "ok" : "FAIL", secs, detail.c_str());
    char head[160];
    std::snprintf(head, sizeof(head), "{\"line\":%d,\"optional\":%s,\"ok\":%s,\"seconds\":%.1f,", s.line, s.optional ? "true" : "false",
                  ok ? "true" : "false", secs);
    r.results.push_back(std::string(head) + "\"step\":\"" + te::JsonEscape(what) + "\",\"detail\":\"" + te::JsonEscape(detail) + "\"}");
    if (!ok) ++r.failures;
}

// Runs the scenario. False = a required step failed (or the run ended).
bool RunSteps(Run& r) {
    for (const te::Step& s : r.scenario.steps) {
        const ULONGLONG t0 = GetTickCount64();
        bool ok = false;
        std::string detail, what;
        switch (s.kind) {
            case te::StepKind::note:
                what = "note " + s.text;
                ok = true;
                detail = s.text;
                break;
            case te::StepKind::sleep:
                what = "sleep " + std::to_string(s.ms);
                ok = Watch(r, s.ms);
                detail = ok ? "slept" : r.end_reason;
                break;
            case te::StepKind::waitready: {
                what = "waitready " + std::to_string(s.seconds);
                const ULONGLONG end = t0 + s.seconds * 1000ull;
                while (!(ok = te::FileExists(r.auto_out + L"\\ready.txt")) && GetTickCount64() < end && Watch(r, 200)) {
                }
                std::string ready;
                te::ReadFileBytes(r.auto_out + L"\\ready.txt", &ready);
                for (char& c : ready)
                    if (c == '\n') c = ' ';
                detail = ok ? "automation channel up: " + ready : (r.end_reason.empty() ? "no ready.txt" : r.end_reason);
                break;
            }
            case te::StepKind::waitlog: {
                what = "waitlog " + std::to_string(s.seconds) + " " + s.text;
                const std::regex re(s.text);
                const ULONGLONG end = t0 + s.seconds * 1000ull;
                while (!(ok = LogMatches(r, re, &detail)) && GetTickCount64() < end && Watch(r, 500)) {
                }
                if (!ok) detail = r.end_reason.empty() || Alive(r.o) ? "no matching log line" : r.end_reason;
                break;
            }
            case te::StepKind::until: {
                what = "until " + s.text + " => " + s.command;
                const std::regex re(s.text);
                const ULONGLONG end = t0 + s.seconds * 1000ull;
                unsigned sent = 0;
                while (!(ok = LogMatches(r, re, &detail)) && GetTickCount64() < end) {
                    std::string d;
                    const bool sent_ok = ModCommand(r, s.command, &d);
                    ++sent;
                    if (!sent_ok) {
                        detail = "command failed: " + d;
                        break;
                    }
                    if (!Watch(r, s.ms)) break;
                }
                if (ok) detail = "after " + std::to_string(sent) + " x '" + s.command + "': " + detail;
                else if (detail.empty()) detail = "no match after " + std::to_string(sent) + " x '" + s.command + "'" +
                                                  (r.end_reason.empty() ? "" : " (" + r.end_reason + ")");
                break;
            }
            case te::StepKind::mod:
                what = s.text;
                ok = ModCommand(r, s.text, &detail);
                break;
        }
        Record(r, s, what.c_str(), ok, detail, t0);
        if (!ok && !Alive(r.o)) return false;
        if (!ok && !r.end_reason.empty() && r.end_reason != "the game exited") return false;
        if (!ok && !s.optional) return false;
        if (Expired(r)) {
            r.end_reason = "run timeout";
            return false;
        }
    }
    return true;
}

std::string PreflightGame(Run& r) {
    if (!te::FileExists(r.game + L"\\ds.exe")) return "no ds.exe in " + te::Narrow(r.game);
    // --no-mod deploys the DLL too: it is what redirects the saves
    if (!te::FileExists(r.args.mod)) return "mod DLL not found: " + te::Narrow(r.args.mod);
    if (r.args.no_mod)
        for (const te::Step& s : r.scenario.steps)
            if (s.kind == te::StepKind::mod || s.kind == te::StepKind::waitready || s.kind == te::StepKind::until)
                return "--no-mod has no automation channel: the scenario may only use sleep, note and waitlog (line " + std::to_string(s.line) + ")";
    const auto running = te::FindProcesses(L"ds.exe");
    if (!running.empty()) return "ds.exe is already running (pid " + std::to_string(running[0]) + "): close it first";
    if (r.args.steam_check && te::FindProcesses(L"steam.exe").empty())
        return "Steam is not running: the game's SteamAPI_Init would fail (start Steam, log in)";
    if (te::FileExists(r.game + L"\\steam_appid.txt")) return "steam_appid.txt exists in the game folder (not ours): refusing";
    return {};
}

std::wstring EnvW(const wchar_t* name) {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf, n) : std::wstring();
}

// The save tree ds.exe uses (docs/hidden-testing.md "Saves"): its "user:" device is
// getenv("USERPROFILE") + "AppData\Local\KojimaProductions\DeathStrandingDC\<id>\"; Steam's
// AutoCloud root is %LOCALAPPDATA%\KojimaProductions. Both must be the same folder.
std::wstring DefaultSaveRoot(std::string* why) {
    const std::wstring local = EnvW(L"LOCALAPPDATA"), profile = EnvW(L"USERPROFILE");
    if (local.empty() || profile.empty()) {
        *why = "LOCALAPPDATA or USERPROFILE is not set: cannot locate the saves";
        return {};
    }
    const std::wstring a = te::FullPath(local + L"\\KojimaProductions"), b = te::FullPath(profile + L"\\AppData\\Local\\KojimaProductions");
    if (_wcsicmp(a.c_str(), b.c_str()) != 0) {
        *why = "%LOCALAPPDATA%\\KojimaProductions and %USERPROFILE%\\AppData\\Local\\KojimaProductions differ (" + te::Narrow(a) + " / " +
               te::Narrow(b) + "): refusing";
        return {};
    }
    return a;
}

std::wstring CloudLogPath() {
    const std::wstring steam = SteamPath();
    return steam.empty() ? std::wstring() : te::FullPath(steam + L"\\logs\\cloud_log.txt");
}

unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

std::string TailFrom(const std::wstring& path, unsigned long long offset) {
    std::string all;
    if (!te::ReadFileBytes(path, &all, 256ull << 20)) return {};
    return offset <= all.size() ? all.substr(static_cast<std::size_t>(offset)) : all;  // rotated: all of it
}

std::string LastLines(const std::string& text, std::size_t n) {
    std::vector<std::string> lines;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(at, nl - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) lines.push_back(l);
        at = nl + 1;
    }
    std::string out;
    for (std::size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i) out += lines[i] + "\n";
    return out;
}

std::string JsonList(const std::vector<std::string>& v) {
    std::string j = "[";
    for (std::size_t i = 0; i < v.size(); ++i) j += (i ? ",\"" : "\"") + te::JsonEscape(v[i]) + "\"";
    return j + "]";
}

// The mod's DllMain marker (automation\saves-redirect.txt, written before ds.exe's own
// code runs) within 10 s, then its "[auto] saves=redirected" log line within 30 s.
// Anything else ends the run before a scenario step can make the game load a save.
bool WaitSavesEvidence(Run& r, std::string* detail) {
    const std::wstring active = r.mod_dir + L"\\automation\\saves-redirect.txt";
    const std::wstring refused = r.mod_dir + L"\\automation\\saves-redirect-failed.txt";
    const ULONGLONG t0 = GetTickCount64();
    const auto refused_text = [&]() {
        std::string t;
        te::ReadFileBytes(refused, &t);
        for (char& c : t)
            if (c == '\n' || c == '\r') c = ' ';
        return t;
    };
    while (!te::FileExists(active)) {
        if (te::FileExists(refused)) return *detail = "the mod REFUSED: " + refused_text(), false;
        if (GetTickCount64() - t0 > 10000) return *detail = "no automation\\saves-redirect.txt within 10 s (mod not loaded?)", false;
        if (!Watch(r, 50)) {
            if (te::FileExists(refused)) return *detail = "the mod REFUSED: " + refused_text(), false;
            return *detail = "the game ended before the save redirect was confirmed (" + r.end_reason + ")", false;
        }
    }
    const double marker_s = static_cast<double>(GetTickCount64() - t0) / 1000.0;
    const std::regex re("\\[auto\\] saves=redirected");
    std::string line;
    while (!LogMatches(r, re, &line)) {
        if (GetTickCount64() - t0 > 30000) return *detail = "no '[auto] saves=redirected' log line within 30 s", false;
        if (!Watch(r, 100)) {
            if (LogMatches(r, re, &line)) break;
            return *detail = "the game ended before the redirect log line (" + r.end_reason + ")", false;
        }
    }
    char head[96];
    std::snprintf(head, sizeof(head), "marker after %.2f s; ", marker_s);
    *detail = head + line;
    return true;
}

int RunGame(Args& a) {
    Run r;
    r.args = a;
    r.run = te::FullPath(a.run_dir);
    r.game = te::FullPath(a.game_dir);
    r.args.mod = te::FullPath(a.mod);
    r.mod_dir = r.game + L"\\stranded_together";
    r.auto_in = r.mod_dir + L"\\automation\\in";
    r.auto_out = r.mod_dir + L"\\automation\\out";
    r.log_path = r.mod_dir + L"\\stranded_together.log";
    const std::wstring scratch = r.mod_dir + L"\\automation\\saves";

    // ---- preflight (nothing changed yet)
    std::string text, err;
    if (!te::ReadFileBytes(te::FullPath(a.scenario), &text) || !te::ParseScenario(text, &r.scenario, &err)) {
        Out("REFUSED: scenario %s: %s", te::Narrow(a.scenario).c_str(), err.empty() ? "unreadable" : err.c_str());
        return 2;
    }
    te::WriteFileAtomic(r.run + L"\\scenario.txt", text);
    const unsigned limit = std::min(a.timeout_s, r.scenario.timeout_s);
    std::string why = PreflightGame(r);
    // --saves <dir>: an explicit save root (tests: a scratch "real" root, also handed to the
    // mod as automation_save_root). Otherwise the user's own.
    std::wstring save_root = a.saves.empty() ? std::wstring() : te::FullPath(a.saves);
    if (why.empty() && a.saves == L"none") why = "--saves none is gone: every run redirects and checks the saves";
    if (why.empty() && save_root.empty()) save_root = DefaultSaveRoot(&why);
    if (!why.empty()) {
        Out("REFUSED: %s", why.c_str());
        te::WriteFileAtomic(r.run + L"\\summary.json", "{\"mode\":\"game\",\"result\":\"refused\",\"reason\":\"" + te::JsonEscape(why) + "\"}\n");
        return 2;
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\StrandedTogetherTestEnv");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        Out("REFUSED: another st_testenv_supervisor run is active");
        return 2;
    }
    Out("game %s, mod %s, scenario %s (%zu steps, role %s, steps limit %u s, game lifetime cap %u s)", te::Narrow(r.game).c_str(),
        te::Narrow(r.args.mod).c_str(), te::Narrow(a.scenario).c_str(), r.scenario.steps.size(), r.scenario.role.c_str(),
        std::min(limit, kMaxRunSeconds - kShutdownReserveSeconds), kMaxRunSeconds);

    // ---- saves: hashed and backed up BEFORE anything else changes
    if (!te::BackupSaves(save_root, r.run, &err)) {
        Out("REFUSED: saves backup: %s", err.c_str());
        if (mutex) ReleaseMutex(mutex), CloseHandle(mutex);
        return 2;
    }
    {
        std::string m;
        te::ReadFileBytes(r.run + L"\\saves-before.txt", &m);
        Out("saves %s: %zu manifest lines hashed, backed up to saves-backup (verified); saves-restore.txt written",
            te::Narrow(save_root).c_str(), static_cast<std::size_t>(std::count(m.begin(), m.end(), '\n')));
    }
    const std::wstring cloud_log = CloudLogPath();
    const unsigned long long cloud_offset = cloud_log.empty() ? 0 : FileSize(cloud_log);

    // ---- back up the game folder, then deploy (the mod, session.ini, the seeded scratch saves)
    r.error_before = te::Sha256File(r.game + L"\\error.txt");
    if (!te::BackupGameFolder(r.game, r.run, &err)) {
        Out("REFUSED: backup: %s", err.c_str());
        if (mutex) ReleaseMutex(mutex), CloseHandle(mutex);
        return 2;
    }
    Out("game folder backed up (restore.txt written before any change)");
    bool deployed = te::EnsureDir(r.mod_dir);
    te::RemoveTree(r.mod_dir + L"\\automation");
    deployed = deployed && te::EnsureDir(r.auto_in) && te::EnsureDir(r.auto_out);
    const std::wstring extra_root = a.saves.empty() ? std::wstring() : save_root;  // the mod knows the user's own root
    // --seed-saves only: another account's saves need ds.exe's save-owner check skipped
    // (automation_foreign_saves=1; the mod does it only in this supervised, redirected run).
    const bool foreign = te::WantForeignSaves(a.seed, a.no_mod);
    deployed = deployed && CopyFileW(r.args.mod.c_str(), (r.game + L"\\winhttp.dll").c_str(), FALSE);
    deployed = deployed && te::WriteFileAtomic(r.mod_dir + L"\\session.ini",
                                               a.no_mod ? te::RedirectOnlyIniText(extra_root)
                                                        : te::SessionIniText(r.scenario.role, a.mute, a.isolation, extra_root, foreign));
    if (deployed && !te::SeedScratch(r.run, scratch, &err)) {
        Out("setup failed: %s", err.c_str());
        deployed = false;
    }
    // --seed-saves: the game loads another save (e.g. a 100% completion one) from the scratch
    // copy; the user's real saves are still backed up, redirected away from and verified.
    if (deployed && !a.seed.empty()) {
        int accounts = 0;
        if (!te::ApplySeedSaves(scratch, te::FullPath(a.seed), &accounts, &err)) {
            Out("setup failed: %s", err.c_str());
            deployed = false;
        } else {
            Out("scratch saves: slots replaced from %s in %d account folder(s); the real saves are untouched",
                te::Narrow(te::FullPath(a.seed)).c_str(), accounts);
        }
    }
    if (deployed) te::WriteFileAtomic(r.run + L"\\saves-scratch-before.txt", te::ManifestWithoutProbe(scratch));
    DeleteFileW((r.mod_dir + L"\\session.last.ini").c_str());
    if (a.no_mod) Out("--no-mod: winhttp.dll only redirects the saves (no session, no overlay, no game hooks)");
    else
        Out("deployed: winhttp.dll %s, session.ini role=%s automation=1 one_shot=1%s%s", te::Sha256File(r.game + L"\\winhttp.dll").substr(0, 16).c_str(),
            r.scenario.role.c_str(), a.isolation ? "" : " automation_isolate=0", foreign ? " automation_foreign_saves=1" : "");
    if (!a.seed.empty() && a.no_mod) Out("--no-mod with --seed-saves: no foreign-saves bypass (another account's saves show as corrupted)");
    Out("scratch saves seeded: %s (the mod redirects %s there)", te::Narrow(scratch).c_str(), te::Narrow(save_root).c_str());
    g_full_steam_env = a.steam_env;
    if (a.steam_env) Out("--steam-env: the child gets Steam's launch variables too");

    int rc = 1;
    bool started = false, redirected = false;
    std::string evidence, foreign_note;
    ULONGLONG hard_end = 0;
    if (deployed && CreatePrivateDesktop(r.o) && CreateJob(r.o, a.ui_limits)) {
        const ULONGLONG launch = GetTickCount64();
        hard_end = launch + kMaxRunSeconds * 1000ull;
        r.deadline = launch + std::min(limit, kMaxRunSeconds - kShutdownReserveSeconds) * 1000ull;
        started = Launch(r.o, r.game + L"\\ds.exe", L"", r.game, true, a.game_stdout ? r.run + L"\\game-stdout.txt" : std::wstring());
    } else {
        Out("setup failed (deploy/seed/desktop/job): restoring");
        rc = 2;
    }
    bool steps_ok = false;
    if (started) {
        redirected = WaitSavesEvidence(r, &evidence);
        Out("saves redirect: %s -- %s", redirected ? "CONFIRMED" : "NOT CONFIRMED", evidence.c_str());
        if (redirected && foreign) {
            // logged by the mod right after "saves=redirected" (Init): where it bypassed, or why not
            const std::regex fre("\\[auto\\] foreign saves: ");
            std::string line;
            for (int i = 0; i < 50 && !LogMatches(r, fre, &line); ++i)
                if (!Watch(r, 100)) break;
            foreign_note = line.empty() ? "no '[auto] foreign saves:' log line" : line;
            Out("foreign saves: %s", foreign_note.c_str());
        }
        if (!redirected) {
            if (Alive(r.o)) KillOwned(r.o, "the save redirect was not confirmed");
            if (r.end_reason.empty() || r.end_reason == "the game exited") r.end_reason = "saves not redirected: " + evidence;
        } else if (!Watch(r, 1000)) {
            Out("the game is gone right after start: %s", r.end_reason.c_str());
        } else {
            steps_ok = RunSteps(r);
        }
        Out("scenario %s%s%s", steps_ok ? "completed" : "stopped", r.end_reason.empty() ? "" : ": ", r.end_reason.c_str());
        // The end of the run (final screenshot, quit, exit) stays inside the hard cap.
        r.deadline = hard_end - 5000;
        if (!r.o.violation && Alive(r.o) && redirected) {
            if (!steps_ok && !a.no_mod) {
                std::string d;
                ModCommand(r, "screenshot zz-final", &d);
                Out("final screenshot: %s", d.c_str());
            }
            if (!r.quit_sent && !a.no_mod) {
                std::string d;
                ModCommand(r, "quit", &d);
            }
            // --no-mod has no quit command
            const ULONGLONG end = std::min<ULONGLONG>(GetTickCount64() + (a.no_mod ? 0 : 30000), hard_end);
            while (Alive(r.o) && GetTickCount64() < end) WaitForSingleObject(r.o.process, 250);
            if (Alive(r.o)) KillOwned(r.o, "did not exit after quit");
        }
        KillOwned(r.o, "end of run (no-op when everything exited)");
        DWORD code = 0;
        GetExitCodeProcess(r.o.process, &code);
        FILETIME c{}, e{}, k{}, u{};
        if (GetProcessTimes(r.o.process, &c, &e, &k, &u)) {
            const auto t = [](const FILETIME& f) { return (static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            r.lifetime_s = static_cast<double>(t(e) - t(c)) / 1e7;
            r.cpu_s = static_cast<double>(t(k) + t(u)) / 1e7;
        }
        r.exit_code = code;
        Out("ds.exe exit code %lu (0x%08lX) after %.2f s alive, %.2f s CPU", code, code, r.lifetime_s, r.cpu_s);
        rc = r.o.violation ? 3 : (steps_ok && r.failures == 0 && redirected ? 0 : 1);
    }
    CloseOwned(r.o);

    // ---- collect
    const std::wstring got = r.run + L"\\game";
    te::EnsureDir(got);
    for (const wchar_t* n : {L"stranded_together.log", L"session.last.ini", L"link-status.json"})
        CopyFileW((r.mod_dir + L"\\" + n).c_str(), (got + L"\\" + n).c_str(), FALSE);
    for (const wchar_t* n : {L"saves-redirect.txt", L"saves-redirect-failed.txt", L"exit-trace.txt"})
        CopyFileW((r.mod_dir + L"\\automation\\" + n).c_str(), (got + L"\\" + n).c_str(), FALSE);
    if (te::DirExists(r.auto_out)) te::CopyTree(r.auto_out, r.run + L"\\automation");
    std::vector<std::string> scratch_changes;
    if (te::DirExists(scratch)) {
        std::string before;
        if (!te::ReadFileBytes(r.run + L"\\saves-scratch-before.txt", &before))
            te::ReadFileBytes(r.run + L"\\saves-before.txt", &before);
        DeleteFileW((scratch + L"\\.st-redirect-probe").c_str());
        const std::string after = te::HashManifest(scratch);
        te::WriteFileAtomic(r.run + L"\\saves-scratch-after.txt", after);
        scratch_changes = te::DiffManifests(before, after);
        for (const std::string& c : scratch_changes) {  // what the game wrote (into the copy): kept for inspection
            const std::wstring rel = te::Widen(c.substr(c.find(' ') + 1));
            if (!rel.empty() && rel.back() != L'\\' && c.rfind("removed ", 0) != 0) {
                const std::wstring dst = r.run + L"\\saves-scratch-changed\\" + rel;
                te::EnsureDir(dst.substr(0, dst.find_last_of(L'\\')));
                CopyFileW((scratch + L"\\" + rel).c_str(), dst.c_str(), FALSE);
            }
        }
        Out("scratch saves: %zu change(s) by the game (absorbed by the redirect)", scratch_changes.size());
        for (const std::string& c : scratch_changes) Out("  scratch %s", c.c_str());
    }
    std::string error_note = "none";
    {
        const std::string now_sha = te::Sha256File(r.game + L"\\error.txt");
        if (!now_sha.empty() && now_sha != r.error_before) {
            CopyFileW((r.game + L"\\error.txt").c_str(), (got + L"\\error.txt").c_str(), FALSE);
            error_note = "NEW error.txt from this run (copied to game\\error.txt)";
            Out("the game wrote a NEW error.txt (crash report): copied to the run folder");
            if (rc == 0) rc = 1;
        }
    }
    std::string exit_trace, refused_reason, game_stdout;
    te::ReadFileBytes(got + L"\\exit-trace.txt", &exit_trace);
    te::ReadFileBytes(got + L"\\saves-redirect-failed.txt", &refused_reason);
    if (te::ReadFileBytes(r.run + L"\\game-stdout.txt", &game_stdout)) Out("game stdout: %zu bytes (game-stdout.txt)", game_stdout.size());
    if (!exit_trace.empty()) Out("exit trace: %s", exit_trace.c_str());

    // ---- restore: the game folder, then the saves (compared by hash, restored if anything differs)
    const te::GuardResult restored = te::RestoreGameFolder(r.run);
    for (const std::string& n : restored.notes) Out("restore: %s", n.c_str());
    Out("game folder restore %s", restored.ok ? "complete" : "INCOMPLETE (see notes)");
    // ---- Steam Cloud: what AutoCloud did with the real folder when the game exited (waited for
    // BEFORE the saves are compared, so the comparison also covers what Steam did after the exit)
    std::vector<std::string> cloud_lines, cloud_uploads;
    bool autocloud_seen = false;
    if (!cloud_log.empty() && started) {
        for (int i = 0; i < 40 && !autocloud_seen; ++i) {  // AutoCloud runs right after the exit
            cloud_lines = te::CloudLogLines(TailFrom(cloud_log, cloud_offset), "1850570");
            for (const std::string& l : cloud_lines)
                autocloud_seen = autocloud_seen || l.find("AutoCloud complete") != std::string::npos ||
                                 l.find("Upload complete") != std::string::npos;
            if (!autocloud_seen) Sleep(500);
        }
        cloud_uploads = te::CloudUploads(cloud_lines);
        std::string all;
        for (const std::string& l : cloud_lines) all += l + "\n";
        te::WriteFileAtomic(r.run + L"\\steam-cloud.txt", all);
        Out("Steam cloud_log: %zu line(s) for app 1850570 since the start (%s), %zu upload line(s)%s", cloud_lines.size(),
            autocloud_seen ? "AutoCloud ran" : "no AutoCloud pass logged", cloud_uploads.size(), cloud_uploads.empty() ? "" : " -- SEE steam-cloud.txt");
        if (!cloud_uploads.empty() && rc == 0) rc = 1;
    }

    // ---- the real saves, compared by hash (restored from saves-backup if anything differs)
    const te::SavesCheck saves = te::CheckAndRestoreSaves(r.run);
    if (saves.unchanged) Out("saves: UNCHANGED (hash manifest identical to the one before the run)");
    else {
        Out("saves: %zu CHANGE(S) in the real save tree -- restored from saves-backup: %s", saves.changes.size(), saves.ok ? "verified" : "INCOMPLETE");
        for (const std::string& c : saves.changes) Out("  real save %s", c.c_str());
        for (const std::string& n : saves.notes) Out("  %s", n.c_str());
        if (rc == 0) rc = 1;
    }

    // ---- summary
    std::string j = "{\"mode\":\"game\",\"result\":\"" + std::string(rc == 0 ? "pass" : rc == 3 ? "violation" : "fail") + "\"";
    j += ",\"end_reason\":\"" + te::JsonEscape(r.end_reason) + "\",\"violation\":" + (r.o.violation ? "true" : "false");
    char life[200];
    std::snprintf(life, sizeof(life), ",\"game_exit_code\":%lu,\"game_alive_s\":%.2f,\"game_cpu_s\":%.2f,\"no_mod\":%s,\"ui_limits\":%s,\"steam_env\":%s,\"isolation\":%s",
                  r.exit_code, r.lifetime_s, r.cpu_s, a.no_mod ? "true" : "false", a.ui_limits ? "true" : "false", a.steam_env ? "true" : "false",
                  a.isolation ? "true" : "false");
    j += life;
    j += ",\"violation_text\":\"" + te::JsonEscape(r.o.violation_text) + "\",\"desktop\":\"" + r.o.desktop_name + "\"";
    j += ",\"input_desktop\":\"" + te::JsonEscape(r.o.initial_input) + "\",\"input_desktop_after\":\"" + te::JsonEscape(InputDesktopName()) + "\"";
    j += ",\"private_desktop_windows\":" + std::to_string(r.o.private_windows) + ",\"private_desktop_titles\":\"" + te::JsonEscape(r.o.private_titles) + "\"";
    j += ",\"failures\":" + std::to_string(r.failures) + ",\"error_txt\":\"" + te::JsonEscape(error_note) + "\"";
    j += ",\"restored\":" + std::string(restored.ok ? "true" : "false") + ",\"restore_notes\":" + JsonList(restored.notes);
    j += ",\"saves_root\":\"" + te::JsonEscape(te::Narrow(save_root)) + "\",\"saves_redirected\":" + (redirected ? "true" : "false");
    j += ",\"saves_evidence\":\"" + te::JsonEscape(evidence) + "\",\"saves_refused\":\"" + te::JsonEscape(refused_reason) + "\"";
    j += ",\"saves_unchanged\":" + std::string(saves.unchanged ? "true" : "false") + ",\"saves_ok\":" + (saves.ok ? "true" : "false");
    j += ",\"save_changes\":" + JsonList(saves.changes) + ",\"saves_restore_notes\":" + JsonList(saves.notes);
    j += ",\"scratch_changes\":" + JsonList(scratch_changes);
    j += ",\"seed_saves\":\"" + te::JsonEscape(te::Narrow(a.seed)) + "\",\"foreign_saves\":" + (foreign ? "true" : "false");
    j += ",\"foreign_saves_evidence\":\"" + te::JsonEscape(foreign_note) + "\"";
    j += ",\"cloud_autocloud_seen\":" + std::string(autocloud_seen ? "true" : "false") + ",\"cloud_lines\":" + std::to_string(cloud_lines.size());
    j += ",\"cloud_uploads\":" + JsonList(cloud_uploads);
    j += ",\"exit_trace\":\"" + te::JsonEscape(exit_trace) + "\",\"game_stdout_tail\":\"" + te::JsonEscape(LastLines(game_stdout, 25)) + "\"";
    j += ",\"steps\":[";
    for (std::size_t i = 0; i < r.results.size(); ++i) j += (i ? ",\n  " : "\n  ") + r.results[i];
    j += "]}\n";
    te::WriteFileAtomic(r.run + L"\\summary.json", j);
    if (!restored.ok && rc == 0) rc = 1;
    Out("run %s (exit %d). Run folder: %s", rc == 0 ? "PASSED" : rc == 3 ? "STOPPED: SAFETY VIOLATION" : "FAILED", rc, te::Narrow(r.run).c_str());
    if (mutex) ReleaseMutex(mutex), CloseHandle(mutex);
    return rc;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Never a console window of our own: write to an inherited stdout (pipe or
    // file), else to the caller's console if it has one.
    HANDLE inherited = GetStdHandle(STD_OUTPUT_HANDLE);
    if (inherited && inherited != INVALID_HANDLE_VALUE && GetFileType(inherited) != FILE_TYPE_UNKNOWN) {
        g_stdout = inherited;
    } else if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_stdout = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_stdout == INVALID_HANDLE_VALUE) g_stdout = nullptr;
    }
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Args a;
    std::string err;
    if (!ParseArgs(a, &err)) {
        Out("usage error: %s", err.c_str());
        return 2;
    }
    if (!a.restore.empty()) {
        OpenLog(te::FullPath(a.restore));
        Out("restore requested for %s", te::Narrow(a.restore).c_str());
        if (!te::FindProcesses(L"ds.exe").empty()) {
            Out("REFUSED: ds.exe is running; restore after it exits");
            return 2;
        }
        const te::GuardResult g = te::RestoreGameFolder(te::FullPath(a.restore));
        for (const std::string& n : g.notes) Out("restore: %s", n.c_str());
        const te::SavesCheck s = te::CheckAndRestoreSaves(te::FullPath(a.restore));
        if (s.ran) Out("saves: %s", s.unchanged ? "unchanged" : s.ok ? "CHANGED, restored and verified" : "CHANGED, restore INCOMPLETE");
        for (const std::string& n : s.notes) Out("saves: %s", n.c_str());
        return g.ok && s.ok ? 0 : 1;
    }
    if (a.run_dir.empty()) {
        Out("usage: --probe <exe> --run-dir <dir> | --game-dir <dir> --mod <dll> --scenario <file> --run-dir <dir> | --restore <run dir>");
        return 2;
    }
    OpenLog(te::FullPath(a.run_dir));
    Out("st_testenv_supervisor (pid %lu) started %s", GetCurrentProcessId(), te::IsoNow().c_str());
    if (!a.probe.empty()) return RunProbe(a);
    if (a.game_dir.empty() || a.mod.empty() || a.scenario.empty()) {
        Out("usage error: --game-dir, --mod and --scenario are required");
        return 2;
    }
    return RunGame(a);
}
