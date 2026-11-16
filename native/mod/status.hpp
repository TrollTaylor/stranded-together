// <mod dir>\link-status.json, read by the launcher's In-game page.
#pragma once
#include <cstdint>
#include <string>

namespace st::status {

struct Snapshot {
    const char* state = "idle";
    const char* transport = "";
    const char* route = "";
    const char* role = "none";
    const char* remote = "none";  // none | spawning | visible | failed
    char peer[48] = {};
    char reason[96] = {};
    char code[96] = {};
    std::uint32_t rtt_ms = 0;
    std::uint64_t rx = 0, tx = 0;
    bool local_valid = false;
    double local_pos[3] = {};
};

void Init(const std::wstring& mod_dir) noexcept;
// Game thread: hands the snapshot to a background writer (no file I/O here).
void Publish(const Snapshot& s) noexcept;
// Writer: atomic replace (tmp + MoveFileEx). Exposed for tools/tests.
void Write(const Snapshot& s) noexcept;

}  // namespace st::status
