#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace st {
namespace {
// A CRITICAL_SECTION (recursive) rather than std::mutex: if a fault ever escapes
// while a line is being written, the SEH handler's own Log must not self-deadlock.
struct Lock {
    CRITICAL_SECTION cs;
    Lock() { InitializeCriticalSection(&cs); }
};
Lock& Mu() {
    static Lock lock;
    return lock;
}
struct Guard {
    Guard() { EnterCriticalSection(&Mu().cs); }
    ~Guard() { LeaveCriticalSection(&Mu().cs); }
};
FILE* g_file = nullptr;
ULONGLONG g_t0 = 0;
bool g_stdout = false;

// Recent-lines ring (under the same lock as the file).
LogLine g_ring[kLogRingLines];
std::uint64_t g_ring_seq = 0;  // seq of the newest line

void RingPush(const char* line) {
    LogLine& slot = g_ring[g_ring_seq % kLogRingLines];
    slot.seq = ++g_ring_seq;
    std::size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
    if (n >= sizeof(slot.text)) n = sizeof(slot.text) - 1;
    std::memcpy(slot.text, line, n);
    slot.text[n] = 0;
}
}  // namespace

void LogInit(const std::wstring& dir) {
    Guard lock;
    if (g_file) return;
    g_t0 = GetTickCount64();
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\stranded_together.log";
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Guard lock;
    const double t = (GetTickCount64() - g_t0) / 1000.0;
    char line[2200];
    std::snprintf(line, sizeof(line), "[%9.3f][%5lu] %s\n", t, GetCurrentThreadId(), buf);
    OutputDebugStringA(line);
    RingPush(line);
    if (g_stdout) {
        std::fputs(line, stdout);
        std::fflush(stdout);
    }
    if (g_file) {
        std::fputs(line, g_file);
        std::fflush(g_file);
    }
}

std::size_t LogCopyRecent(std::uint64_t after_seq, LogLine* out, std::size_t max) {
    if (!out || !max) return 0;
    Guard lock;
    const std::uint64_t newest = g_ring_seq;
    const std::uint64_t oldest = newest > kLogRingLines ? newest - kLogRingLines + 1 : 1;
    std::uint64_t first = after_seq + 1 > oldest ? after_seq + 1 : oldest;
    if (newest >= first && newest - first + 1 > max) first = newest - max + 1;  // keep the newest `max`
    std::size_t n = 0;
    for (std::uint64_t s = first; s <= newest && n < max; ++s) {
        // slot for seq s is (s - 1) % ring (RingPush stores seq k at index (k - 1) % ring)
        out[n++] = g_ring[(s - 1) % kLogRingLines];
    }
    return n;
}

std::uint64_t LogNewestSeq() {
    Guard lock;
    return g_ring_seq;
}

void LogToStdout(bool on) {
    if (!g_t0) g_t0 = GetTickCount64();
    g_stdout = on;
}

void LogShutdown() {
    Guard lock;
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

}  // namespace st
