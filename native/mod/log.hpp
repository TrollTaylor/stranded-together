#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace st {

// Opens <game dir>/stranded_together/stranded_together.log (truncated each launch).
void LogInit(const std::wstring& dir);
// printf-style, thread-safe, flushed per line, timestamped with ms since start.
void Log(const char* fmt, ...);
void LogShutdown();
// For hooks on game worker threads (damage and bullet detours): the line is only formatted into a small ring (no file I/O,
// no flush on the game's worker thread); LogFlushLater, called from a game-thread tick, writes the waiting lines with the
// time and thread they were made on. A full ring drops the line (counted, reported by the next flush).
void LogLater(const char* fmt, ...);
void LogFlushLater();
// Also print every line to stdout (console tools).
void LogToStdout(bool on);

// In-memory ring of the most recent lines (for the in-game debug menu's Log
// tab). Every Log() line gets a sequence number starting at 1; lines longer
// than the slot are truncated.
struct LogLine {
    std::uint64_t seq = 0;
    char text[480] = {};  // "[  12.345][ 1234] message" without the newline
};
constexpr std::size_t kLogRingLines = 512;
// Copies up to `max` lines with seq > after_seq, oldest first; returns the count.
// Lines that already left the ring are skipped silently.
std::size_t LogCopyRecent(std::uint64_t after_seq, LogLine* out, std::size_t max);
// Sequence number of the newest line (0 before the first line).
std::uint64_t LogNewestSeq();

}  // namespace st
