// Stranded Together launcher core: session activation, cleanup and the
// background helper (see core_session.hpp).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core_session.hpp"

#include "core_log.hpp"

namespace st::core {
namespace {

constexpr char kRecordProduct[] = "stranded-together-session";

bool AliveFromRecord(Env& env, std::int64_t pid, const std::string& start_text) {
    if (pid <= 0) return false;
    if (static_cast<std::uint32_t>(pid) == env.CurrentPid()) return false;
    std::int64_t start = 0;
    if (start_text.empty() || !ParseUtc(start_text, start)) return env.ProcessAlive(static_cast<std::uint32_t>(pid));
    return ProcessAliveSince(env, static_cast<std::uint32_t>(pid), start);
}

std::string ProcessStartText(Env& env, std::uint32_t pid) {
    std::int64_t ms = 0;
    return pid && env.ProcessStartMs(pid, ms) ? FormatUtcRoundTrip(ms) : std::string();
}

// Under the mutex: the read-modify-write of an active record with `session_id`.
template <typename F>
bool UpdateRecord(Env&, const InstancePaths& paths, const std::string& session_id, F&& update) {
    PlayMutex mutex(paths.game);
    if (!mutex.Lock()) return false;
    json::Value r;
    if (!ReadSessionRecord(paths, r) || !RecordIsActive(r) || r.Str("session_id") != session_id) return false;
    update(r);
    std::string error;
    return SaveSessionRecord(paths, r, error);
}

// Cleanup body; the caller holds the mutex.
CleanupResult CleanupLocked(Env& env, const InstancePaths& paths) {
    CleanupResult out;
    if (AnyGameRunningIn(env, paths.game)) {
        out.message = "DEATH STRANDING is still running from this folder. Close it first.";
        return out;
    }
    out.ran = true;
    json::Value record;
    const bool have_record = ReadSessionRecord(paths, record);
    if (IsFile(paths.session_ini)) {
        std::string text, hex;
        ReadTextShared(paths.session_ini, text, 1u << 20);
        Sha256FileHex(paths.session_ini, hex);
        const bool ours = (have_record && !record.Str("ini_sha256").empty() && hex == record.Str("ini_sha256")) || IsGeneratedIniText(text);
        if (ours && !IsReparsePoint(paths.session_ini)) {
            out.ini_removed = DeleteFileChecked(paths.session_ini);
            if (!out.ini_removed) out.message = "session.ini could not be deleted (is it open in another program?).";
        } else {
            out.ini_kept = true;
            out.message = "session.ini was not written by this launcher, so it was left in place.";
        }
    }
    // The mod's last status is meaningless once its game is gone.
    if (IsFile(paths.link_status)) {
        std::string text;
        ReadTextShared(paths.link_status, text, 1u << 20);
        const json::Value status = ParseStatusText(text, 0);
        const std::int64_t pid = status.is_object() ? status.Int("pid", 0) : 0;
        if (pid <= 0 || !env.ProcessAlive(static_cast<std::uint32_t>(pid))) DeleteFileChecked(paths.link_status);
    }
    if (have_record && RecordIsActive(record)) {
        record["active"] = false;
        record["ended_utc"] = FormatUtcRoundTrip(env.NowMs());
        std::string error;
        SaveSessionRecord(paths, record, error);
    }
    LauncherLog(env, paths, std::string("cleanup ini_removed=") + (out.ini_removed ? "1" : "0") + " ini_kept=" + (out.ini_kept ? "1" : "0"));
    return out;
}

}  // namespace

// ---- PlayMutex --------------------------------------------------------------------------
PlayMutex::PlayMutex(const std::wstring& game_root) { handle_ = CreateMutexW(nullptr, FALSE, PlayMutexName(game_root).c_str()); }

PlayMutex::~PlayMutex() {
    if (locked_) ReleaseMutex(static_cast<HANDLE>(handle_));
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

bool PlayMutex::Lock(unsigned long timeout_ms) {
    if (!handle_) return false;
    const DWORD r = WaitForSingleObject(static_cast<HANDLE>(handle_), timeout_ms);
    locked_ = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    return locked_;
}

// ---- session-record.json --------------------------------------------------------
bool ReadSessionRecord(const InstancePaths& paths, json::Value& out) {
    std::string text;
    if (!IsFile(paths.record) || !ReadTextShared(paths.record, text, 1u << 20)) return false;
    json::Value v;
    if (!json::Parse(text, v) || !v.is_object() || v.Int("v", 0) != 1 || v.Str("product") != kRecordProduct) return false;
    out = std::move(v);
    return true;
}

bool SaveSessionRecord(const InstancePaths& paths, const json::Value& record, std::string& error) {
    if (HasReparsePoint(paths.record, paths.game)) {
        error = "Filesystem links are not supported in the stranded_together folder.";
        return false;
    }
    return WriteAtomic(paths.record, json::Dump(record) + "\n", true, error);
}

bool RecordIsActive(const json::Value& record) { return record.is_object() && record.Bool("active", false); }

bool SessionOwnerLive(Env& env, const json::Value& record) {
    if (!RecordIsActive(record)) return false;
    return AliveFromRecord(env, record.Int("launcher_pid", 0), record.Str("launcher_start_utc")) ||
           AliveFromRecord(env, record.Int("watchdog_pid", 0), record.Str("watchdog_start_utc"));
}

// ---- Activation ---------------------------------------------------------------------
bool ActivateSession(Env& env, const InstancePaths& paths, const SessionIni& ini, std::string& session_id_out, std::string& error) {
    session_id_out.clear();
    std::string text;
    if (!MakeSessionIniText(ini, text, error)) {
        error = "The session settings are invalid: " + error + ".";
        return false;
    }
    if (!IsDir(paths.mod) && !CreateDirectories(paths.mod)) {
        error = "Could not create the stranded_together folder in the game folder. Is the folder read-only?";
        return false;
    }
    if (HasReparsePoint(paths.mod, paths.game)) {
        error = "The stranded_together folder is a link (junction or symbolic link). Make it a normal folder.";
        return false;
    }
    PlayMutex mutex(paths.game);
    if (!mutex.Lock()) {
        error = "Another Stranded Together window is busy with this game folder. Try again in a moment.";
        return false;
    }
    if (AnyGameRunningIn(env, paths.game)) {
        error = "DEATH STRANDING is already running. Close it first, then start co-op from here.";
        return false;
    }
    json::Value old;
    if (ReadSessionRecord(paths, old) && RecordIsActive(old) && SessionOwnerLive(env, old)) {
        error = "A co-op session is already starting from another Stranded Together window.";
        return false;
    }
    const CleanupResult leftover = CleanupLocked(env, paths);   // a stale session.ini of an earlier run
    if (leftover.ini_kept) {
        error = "There is a session.ini in the stranded_together folder that this launcher did not write (for example from tools/deploy.ps1 -Session). Remove it with Debug > Clear session, then try again.";
        return false;
    }
    const std::string id = RandomHex(16);
    json::Value r = json::Value::Object();
    r["v"] = 1;
    r["product"] = kRecordProduct;
    r["session_id"] = id;
    r["active"] = true;
    r["role"] = ini.role;
    r["transport"] = ini.transport;
    r["ini_sha256"] = Sha256Hex(text);
    r["created_utc"] = FormatUtcRoundTrip(env.NowMs());
    r["launcher_pid"] = static_cast<std::int64_t>(env.CurrentPid());
    const std::int64_t self_start = env.CurrentStartMs();
    r["launcher_start_utc"] = self_start ? FormatUtcRoundTrip(self_start) : std::string();
    r["watchdog_pid"] = 0;
    r["watchdog_start_utc"] = "";
    if (!SaveSessionRecord(paths, r, error)) {
        error = "Could not write the session record: " + error;
        return false;
    }
    if (!WriteAtomic(paths.session_ini, text, true, error)) {
        error = "Could not write session.ini: " + error;
        r["active"] = false;
        std::string ignored;
        SaveSessionRecord(paths, r, ignored);
        return false;
    }
    session_id_out = id;
    LauncherLog(env, paths, "activate role=" + ini.role + " transport=" + ini.transport);
    return true;
}

// ---- Cleanup ------------------------------------------------------------------------
CleanupResult CleanupSession(Env& env, const InstancePaths& paths) {
    PlayMutex mutex(paths.game);
    if (!mutex.Lock()) {
        CleanupResult out;
        out.message = "Another Stranded Together window is busy with this game folder.";
        return out;
    }
    return CleanupLocked(env, paths);
}

bool StaleSessionPending(Env& env, const InstancePaths& paths) {
    json::Value r;
    const bool active = ReadSessionRecord(paths, r) && RecordIsActive(r);
    if (!active) return IsFile(paths.session_ini) && !AnyGameRunningIn(env, paths.game);
    return !AnyGameRunningIn(env, paths.game) && !SessionOwnerLive(env, r);
}

// ---- The background helper ---------------------------------------------------------------
bool SetSessionOwner(Env& env, const InstancePaths& paths, const std::string& session_id) {
    return UpdateRecord(env, paths, session_id, [&](json::Value& r) {
        r["launcher_pid"] = static_cast<std::int64_t>(env.CurrentPid());
        const std::int64_t start = env.CurrentStartMs();
        r["launcher_start_utc"] = start ? FormatUtcRoundTrip(start) : std::string();
    });
}

bool StartWatchdogFor(Env& env, const InstancePaths& paths, const std::string& session_id) {
    std::uint32_t pid = 0;
    if (!env.StartWatchdog(paths.game, session_id, pid) || pid == 0) {
        LauncherLog(env, paths, "watchdog=failed");
        return false;
    }
    const std::string start = ProcessStartText(env, pid);
    UpdateRecord(env, paths, session_id, [&](json::Value& r) {
        const std::int64_t current = r.Int("watchdog_pid", 0);
        if (current && current != static_cast<std::int64_t>(pid) && AliveFromRecord(env, current, r.Str("watchdog_start_utc"))) return;
        r["watchdog_pid"] = static_cast<std::int64_t>(pid);
        r["watchdog_start_utc"] = start;
    });
    LauncherLog(env, paths, "watchdog=started pid=" + std::to_string(pid));
    return true;
}

int RunWatchdog(Env& env, const std::wstring& game_dir, const std::string& session_id) {
    const InstancePaths paths = GetInstancePaths(game_dir);
    const std::uint32_t self = env.CurrentPid();
    {
        PlayMutex mutex(paths.game);
        if (!mutex.Lock()) return 0;
        json::Value r;
        if (!ReadSessionRecord(paths, r) || !RecordIsActive(r) || r.Str("session_id") != session_id) return 0;
        const std::int64_t current = r.Int("watchdog_pid", 0);
        if (current && current != static_cast<std::int64_t>(self) && AliveFromRecord(env, current, r.Str("watchdog_start_utc"))) {
            LauncherLog(env, paths, "watch=second_helper_exits");
            return 0;
        }
        r["watchdog_pid"] = static_cast<std::int64_t>(self);
        const std::int64_t start = env.CurrentStartMs();
        r["watchdog_start_utc"] = start ? FormatUtcRoundTrip(start) : std::string();
        std::string error;
        SaveSessionRecord(paths, r, error);
    }
    LauncherLog(env, paths, "watch=start");
    const std::int64_t began = env.NowMs();
    bool seen = false;
    std::int64_t gone_since = 0;
    for (;;) {
        json::Value r;
        if (!ReadSessionRecord(paths, r) || !RecordIsActive(r) || r.Str("session_id") != session_id) {
            LauncherLog(env, paths, "watch=session_over");
            return 0;
        }
        const bool running = AnyGameRunningIn(env, paths.game);
        if (running) {
            seen = true;
            gone_since = 0;
        } else if (seen) {
            if (!gone_since) gone_since = env.NowMs();
            if (env.NowMs() - gone_since >= kWatchRestartGraceMs) {
                LauncherLog(env, paths, "watch=game_exited");
                for (int attempt = 0; attempt < 10; ++attempt) {
                    if (CleanupSession(env, paths).ran) return 0;
                    env.SleepMs(1000);
                }
                return 1;
            }
        } else if (env.NowMs() - began >= kWatchStartTimeoutMs) {
            // The game never started (Steam failed, the player gave up): leave the folder vanilla.
            LauncherLog(env, paths, "watch=game_never_started");
            CleanupSession(env, paths);
            return 0;
        }
        env.SleepMs(1000);
    }
}

}  // namespace st::core
