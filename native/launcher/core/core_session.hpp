// Stranded Together launcher core: one co-op session in one game folder.
// The pattern is Diamond Dogs Together's core_session, slimmed to one file:
//  - session-record.json (v1) records the session; the SHA-256 of session.ini
//    is recorded BEFORE the file is written;
//  - everything that changes the session runs under the per-folder mutex
//    Local\stranded-together-play-<16 hex>;
//  - cleanup deletes session.ini only when it is ours (its hash matches the
//    record, or it starts with our header) and never while ds.exe runs from the
//    folder, so the game runs vanilla when started without the launcher;
//  - the background helper (--watch) waits for the game and cleans up after it
//    exits, also when the window was closed.
// Dropped from DDT: the save handoff, relaunches, marker files, coop.ini.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core_env.hpp"
#include "core_json.hpp"
#include "core_paths.hpp"
#include "core_text.hpp"

namespace st::core {

class PlayMutex {
public:
    explicit PlayMutex(const std::wstring& game_root);
    ~PlayMutex();
    PlayMutex(const PlayMutex&) = delete;
    PlayMutex& operator=(const PlayMutex&) = delete;
    bool Lock(unsigned long timeout_ms = 15000);
    bool locked() const { return locked_; }

private:
    void* handle_ = nullptr;
    bool locked_ = false;
};

// ---- session-record.json --------------------------------------------------------
bool ReadSessionRecord(const InstancePaths& paths, json::Value& out);   // v == 1 and our product only
bool SaveSessionRecord(const InstancePaths& paths, const json::Value& record, std::string& error);
bool RecordIsActive(const json::Value& record);
// The window or the helper recorded in an active record is still running
// (the calling process never counts).
bool SessionOwnerLive(Env& env, const json::Value& record);

// ---- Activation ---------------------------------------------------------------------
// Writes the record (active, role, transport, the hash of the text) and then
// session.ini. Refuses while ds.exe runs from the folder. A leftover session
// that is not live is cleaned up first.
bool ActivateSession(Env& env, const InstancePaths& paths, const SessionIni& ini, std::string& session_id_out, std::string& error);

// ---- Cleanup ------------------------------------------------------------------------
struct CleanupResult {
    bool ran = false;          // false: refused (the game runs, or the folder is busy)
    bool ini_removed = false;
    bool ini_kept = false;     // a session.ini that is not ours stays
    std::string message;
};
// Deletes session.ini (when ours) and link-status.json (when its writer is
// gone) and marks the record inactive. Never while ds.exe runs from the folder.
CleanupResult CleanupSession(Env& env, const InstancePaths& paths);
// An active record whose game is not running and whose window and helper are
// gone (the launcher was closed, the PC restarted...).
bool StaleSessionPending(Env& env, const InstancePaths& paths);

// ---- The background helper ---------------------------------------------------------------
// StrandedTogether.exe --watch <session_id>: claims the helper role (a second
// helper exits), waits for the game (up to kWatchStartTimeoutMs), waits for it
// to exit (a restart within kWatchRestartGraceMs counts as the same game) and
// cleans up. Returns the process exit code.
inline constexpr std::int64_t kWatchStartTimeoutMs = 10 * 60 * 1000;
inline constexpr std::int64_t kWatchRestartGraceMs = 8000;
int RunWatchdog(Env& env, const std::wstring& game_dir, const std::string& session_id);
// Starts the helper and records it. False when it could not start.
bool StartWatchdogFor(Env& env, const InstancePaths& paths, const std::string& session_id);
// Records the window that owns the session (launcher_pid / start).
bool SetSessionOwner(Env& env, const InstancePaths& paths, const std::string& session_id);

}  // namespace st::core
