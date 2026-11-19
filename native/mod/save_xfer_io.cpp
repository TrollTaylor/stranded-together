// STX1's Windows side (save_xfer.hpp "Windows wrappers"): the host's read-only reader and the guest's writes below
// its redirected save folder. No game calls; safe on any thread (ReadSaveSnapshot blocks: never the game thread).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>

#include "save_xfer.hpp"

namespace st::stx {
namespace {

static_assert(kSaveReadAccess == GENERIC_READ, "the reader's access is GENERIC_READ");
static_assert(kSaveReadShare == (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE), "the reader shares everything");

// The one CreateFileW of a save path for reading. Every open asserts the access pair first.
HANDLE OpenChecked(const std::wstring& path, unsigned long access, unsigned long share) noexcept {
    if (!OpenAccessAllowed(access, share)) return INVALID_HANDLE_VALUE;
    return CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

std::wstring Lower(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o) c = static_cast<wchar_t>(std::towlower(c));
    for (wchar_t& c : o)
        if (c == L'/') c = L'\\';
    while (o.size() > 3 && o.back() == L'\\') o.pop_back();
    return o;
}

// `child` is `parent` or below it (case-insensitive, whole components).
bool Inside(const std::wstring& child, const std::wstring& parent) {
    const std::wstring c = Lower(child), p = Lower(parent);
    if (p.empty() || c.size() < p.size() || c.compare(0, p.size(), p) != 0) return false;
    return c.size() == p.size() || c[p.size()] == L'\\' || p.back() == L'\\';
}

bool Absolute(const std::wstring& p) { return p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'); }

bool ReadAllHandle(HANDLE h, std::vector<std::uint8_t>& out, std::uint32_t max_bytes, ReadStatus& status) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
        status = ReadStatus::read_failed;
        return false;
    }
    if (static_cast<unsigned long long>(size.QuadPart) > max_bytes) {
        status = ReadStatus::too_large;
        return false;
    }
    try {
        out.assign(static_cast<std::size_t>(size.QuadPart), 0);
    } catch (...) {
        status = ReadStatus::read_failed;
        return false;
    }
    std::size_t at = 0;
    while (at < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(out.size() - at > (1u << 20) ? (1u << 20) : out.size() - at);
        if (!ReadFile(h, out.data() + at, want, &got, nullptr) || got == 0) {
            status = ReadStatus::read_failed;
            return false;
        }
        at += got;
    }
    // Nothing more: the file grew while it was read, which the snapshot rule catches anyway.
    std::uint8_t extra = 0;
    DWORD got = 0;
    if (ReadFile(h, &extra, 1, &got, nullptr) && got != 0) {
        status = ReadStatus::unstable;
        return false;
    }
    status = ReadStatus::ok;
    return true;
}

bool EnsureDirs(const std::wstring& root, const std::wstring& rel_dir) {
    std::wstring at = root;
    std::size_t i = 0;
    while (i < rel_dir.size()) {
        std::size_t sep = rel_dir.find(L'\\', i);
        if (sep == std::wstring::npos) sep = rel_dir.size();
        at += L"\\" + rel_dir.substr(i, sep - i);
        if (!CreateDirectoryW(at.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        const DWORD a = GetFileAttributesW(at.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        i = sep + 1;
    }
    return true;
}

std::string Err(const char* what) {
    char b[160];
    wsprintfA(b, "%s (error %lu)", what, GetLastError());
    return b;
}

// The checks every guest write goes through: target usable, path allowed. *full = <scratch>\<rel>.
bool GuestTarget(const SandboxTarget& t, const std::wstring& rel, GuestPath want, std::wstring* full, std::string* why) {
    const std::string problem = SandboxTargetProblem(t);
    if (!problem.empty()) {
        if (why) *why = problem;
        return false;
    }
    const GuestPath kind = ClassifyGuestPath(rel);
    if (kind == GuestPath::invalid || (want != GuestPath::invalid && kind != want)) {
        if (why) *why = "not a path the guest may write (only incoming\\<sha>.part and DeathStrandingDC\\<account>\\manualsave<N>\\checkpoint.dat)";
        return false;
    }
    std::wstring root = t.scratch_root;
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    *full = root + L"\\" + rel;
    for (const std::wstring& real : t.real_roots)
        if (!real.empty() && Inside(*full, real)) {
            if (why) *why = "the target is below a real save root";
            return false;
        }
    const std::size_t slash = rel.rfind(L'\\');
    if (slash != std::wstring::npos && !EnsureDirs(root, rel.substr(0, slash))) {
        if (why) *why = Err("could not create the folder below the redirected save folder");
        return false;
    }
    return true;
}

}  // namespace

bool OpenAccessAllowed(unsigned long access, unsigned long share) noexcept { return access == kSaveReadAccess && share == kSaveReadShare; }

const char* ReadStatusName(ReadStatus s) noexcept {
    switch (s) {
        case ReadStatus::ok: return "ok";
        case ReadStatus::open_failed: return "open_failed";
        case ReadStatus::too_large: return "too_large";
        case ReadStatus::read_failed: return "read_failed";
        case ReadStatus::unstable: return "unstable";
        case ReadStatus::busy: return "busy (the game is writing that slot: checkpoint.dattemp exists)";
    }
    return "?";
}

ReadStatus ReadSaveFileReadOnly(const std::wstring& path, std::vector<std::uint8_t>& out, std::uint32_t max_bytes) {
    out.clear();
    // The game writes <slot>\checkpoint.dattemp, then renames it over checkpoint.dat; a handle of ours open at that moment
    // makes its rename fail (save_xfer.hpp). While the temp file exists the slot is not opened at all.
    if (GetFileAttributesW((path + L"temp").c_str()) != INVALID_FILE_ATTRIBUTES) return ReadStatus::busy;
    const HANDLE h = OpenChecked(path, kSaveReadAccess, kSaveReadShare);
    if (h == INVALID_HANDLE_VALUE) return ReadStatus::open_failed;
    ReadStatus status = ReadStatus::read_failed;
    if (!ReadAllHandle(h, out, max_bytes, status)) out.clear();
    CloseHandle(h);
    return status;
}

ReadStatus ReadSaveSnapshot(const std::wstring& path, std::vector<std::uint8_t>& out, unsigned settle_ms, unsigned tries) {
    ReadStatus last = ReadStatus::unstable;
    for (unsigned attempt = 0; attempt < (tries ? tries : 1); ++attempt) {
        std::vector<std::uint8_t> a, b;
        last = ReadSaveFileReadOnly(path, a);
        if (last == ReadStatus::ok) {
            Sleep(settle_ms);
            last = ReadSaveFileReadOnly(path, b);
            if (last == ReadStatus::ok) {
                if (a.size() == b.size() && Sha256Of(a.data(), a.size()) == Sha256Of(b.data(), b.size())) {
                    out = std::move(b);
                    return ReadStatus::ok;
                }
                last = ReadStatus::unstable;
            }
        }
        if (last == ReadStatus::too_large) break;
        Sleep(settle_ms);
    }
    out.clear();
    return last;
}

std::string SandboxTargetProblem(const SandboxTarget& t) {
    if (!t.redirected) return "the save redirect is not confirmed: nothing is written";
    if (t.scratch_root.empty() || !Absolute(t.scratch_root)) return "no redirected save folder";
    for (const std::wstring& real : t.real_roots)
        if (!real.empty() && (Inside(t.scratch_root, real) || Inside(real, t.scratch_root)))
            return "the redirected save folder and a real save root overlap";
    const DWORD a = GetFileAttributesW(t.scratch_root.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return "the redirected save folder does not exist";
    return {};
}

bool WriteGuestSaveFile(const SandboxTarget& target, const std::wstring& rel, const std::uint8_t* data, std::size_t size, std::string* why) {
    if (!data || size == 0 || size > kMaxSaveBytes) {
        if (why) *why = "nothing to write, or more than the largest save";
        return false;
    }
    std::wstring full;
    if (!GuestTarget(target, rel, GuestPath::invalid, &full, why)) return false;
    const std::wstring tmp = full + L".st-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (why) *why = Err("could not create the temp file");
        return false;
    }
    std::size_t at = 0;
    bool ok = true;
    while (ok && at < size) {
        DWORD wrote = 0;
        const DWORD piece = static_cast<DWORD>(size - at > (1u << 20) ? (1u << 20) : size - at);
        ok = WriteFile(h, data + at, piece, &wrote, nullptr) && wrote == piece;
        at += wrote;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) {
        if (why) *why = Err("could not write the temp file");
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), full.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (why) *why = Err("could not move the temp file into place");
        DeleteFileW(tmp.c_str());
        return false;
    }
    std::vector<std::uint8_t> back;
    if (ReadSaveFileReadOnly(full, back) != ReadStatus::ok || back.size() != size || Sha256Of(back.data(), back.size()) != Sha256Of(data, size)) {
        if (why) *why = "the written file reads back different (SHA-256)";
        return false;
    }
    return true;
}

// ---- SandboxPartStore ----------------------------------------------------------------------------------------
bool SandboxPartStore::Open(const Sha256& sha, std::uint32_t size, std::uint32_t& have) noexcept {
    try {
        have = 0;
        const std::wstring rel = IncomingPartRel(sha);
        std::wstring full;
        if (!GuestTarget(target_, rel, GuestPath::incoming_part, &full, &error_)) return false;
        // Another transfer's part never stays next to this one.
        if (!rel_.empty() && rel_ != rel) Remove();
        rel_ = rel;
        size_ = size;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fa)) {
            const unsigned long long n = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            if (n > size) {
                if (!DeleteFileW(full.c_str())) {
                    error_ = Err("could not delete an oversized part file");
                    return false;
                }
            } else {
                have = static_cast<std::uint32_t>(n);
            }
        }
        return true;
    } catch (...) {
        error_ = "exception";
        return false;
    }
}

bool SandboxPartStore::Append(std::uint32_t offset, const std::uint8_t* data, std::size_t len) noexcept {
    try {
        std::wstring full;
        if (rel_.empty() || !GuestTarget(target_, rel_, GuestPath::incoming_part, &full, &error_)) return false;
        if (static_cast<std::uint64_t>(offset) + len > size_) {
            error_ = "past the offered size";
            return false;
        }
        HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            error_ = Err("could not open the part file");
            return false;
        }
        LARGE_INTEGER cur{};
        bool ok = GetFileSizeEx(h, &cur) && cur.QuadPart == static_cast<LONGLONG>(offset);   // in order only
        LARGE_INTEGER zero{};
        ok = ok && SetFilePointerEx(h, zero, nullptr, FILE_END);
        DWORD wrote = 0;
        ok = ok && WriteFile(h, data, static_cast<DWORD>(len), &wrote, nullptr) && wrote == len;
        CloseHandle(h);
        if (!ok) error_ = Err("could not append to the part file");
        return ok;
    } catch (...) {
        error_ = "exception";
        return false;
    }
}

bool SandboxPartStore::ReadAll(std::vector<std::uint8_t>& out) noexcept {
    try {
        std::wstring full;
        if (rel_.empty() || !GuestTarget(target_, rel_, GuestPath::incoming_part, &full, &error_)) return false;
        return ReadSaveFileReadOnly(full, out) == ReadStatus::ok;
    } catch (...) {
        error_ = "exception";
        return false;
    }
}

void SandboxPartStore::Remove() noexcept {
    try {
        std::wstring full;
        if (rel_.empty() || !GuestTarget(target_, rel_, GuestPath::incoming_part, &full, &error_)) return;
        DeleteFileW(full.c_str());
        rel_.clear();
    } catch (...) {
    }
}

}  // namespace st::stx
