#include "status.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

namespace st::status {
namespace {

std::wstring g_path, g_tmp;
SRWLOCK g_lock = SRWLOCK_INIT;
Snapshot g_pending;
bool g_has_pending = false;
HANDLE g_event = nullptr;

DWORD WINAPI WriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_event, INFINITE);
        Snapshot s;
        bool have = false;
        AcquireSRWLockExclusive(&g_lock);
        if (g_has_pending) {
            s = g_pending;
            g_has_pending = false;
            have = true;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (have) Write(s);
    }
}

void AppendEscaped(std::string& out, const char* s) {
    out += '"';
    for (; s && *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

}  // namespace

void Init(const std::wstring& mod_dir) noexcept {
    g_path = mod_dir + L"\\link-status.json";
    g_tmp = g_path + L".tmp";
    if (!g_event) {
        g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_event) {
            HANDLE t = CreateThread(nullptr, 0, &WriterThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
}

void Publish(const Snapshot& s) noexcept {
    if (!g_event) return;
    AcquireSRWLockExclusive(&g_lock);
    g_pending = s;
    g_has_pending = true;
    ReleaseSRWLockExclusive(&g_lock);
    SetEvent(g_event);
}

void Write(const Snapshot& s) noexcept {
    if (g_path.empty()) return;
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char time[40];
    std::snprintf(time, sizeof(time), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    std::string j;
    j.reserve(512);
    char num[96];
    j += "{\"v\":1,\"pid\":";
    j += std::to_string(GetCurrentProcessId());
    j += ",\"state\":";
    AppendEscaped(j, s.state);
    j += ",\"transport\":";
    AppendEscaped(j, s.transport);
    j += ",\"route\":";
    AppendEscaped(j, s.route);
    j += ",\"role\":";
    AppendEscaped(j, s.role);
    j += ",\"peer\":";
    AppendEscaped(j, s.peer);
    j += ",\"rtt_ms\":";
    j += std::to_string(s.rtt_ms);
    j += ",\"remote\":";
    AppendEscaped(j, s.remote);
    j += ",\"reason\":";
    AppendEscaped(j, s.reason);
    j += ",\"code\":";
    AppendEscaped(j, s.code);
    j += ",\"rx\":";
    j += std::to_string(s.rx);
    j += ",\"tx\":";
    j += std::to_string(s.tx);
    j += ",\"local\":";
    if (s.local_valid) {
        std::snprintf(num, sizeof(num), "[%.2f,%.2f,%.2f]", s.local_pos[0], s.local_pos[1], s.local_pos[2]);
        j += num;
    } else {
        j += "null";
    }
    j += ",\"time_utc\":";
    AppendEscaped(j, time);
    j += "}\n";

    HANDLE h = CreateFileW(g_tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, j.data(), static_cast<DWORD>(j.size()), &written, nullptr);
    CloseHandle(h);
    if (ok && written == j.size()) MoveFileExW(g_tmp.c_str(), g_path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}  // namespace st::status
