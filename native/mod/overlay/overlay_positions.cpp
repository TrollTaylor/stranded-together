#include "overlay_positions.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace st::overlay {
namespace {

bool IsSep(char c) { return c == ' ' || c == '\t' || c == ',' || c == ';' || c == '(' || c == ')' || c == '\r'; }

// Reads one number at p (after separators); advances p. False on no number.
bool ReadNumber(const char*& p, double& out) {
    while (*p && IsSep(*p)) ++p;
    if (!*p) return false;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p || !std::isfinite(v)) return false;
    // The number must end at a separator or the end of the text ("12abc" is not a number).
    if (*end && !IsSep(*end) && *end != '\n') return false;
    out = v;
    p = end;
    return true;
}

}  // namespace

std::string CleanName(const std::string& name, const std::string& fallback) {
    std::string s;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) continue;
        s.push_back(c);
    }
    const auto b = s.find_first_not_of(' ');
    const auto e = s.find_last_not_of(' ');
    s = b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    if (s.size() > 64) s.resize(64);
    return s.empty() ? fallback : s;
}

std::vector<SavedPosition> ParsePositions(const std::string& text) {
    std::vector<SavedPosition> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') {
            if (nl == text.size()) break;
            continue;
        }
        const char* p = line.c_str() + first;
        SavedPosition sp;
        if (ReadNumber(p, sp.x) && ReadNumber(p, sp.y) && ReadNumber(p, sp.z)) {
            while (*p && IsSep(*p)) ++p;
            sp.name = CleanName(p, "position " + std::to_string(out.size() + 1));
            out.push_back(sp);
        }
        if (nl == text.size()) break;
    }
    return out;
}

std::string FormatCoordinates(double x, double y, double z) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.3f %.3f %.3f", x, y, z);
    return buf;
}

std::string FormatPositions(const std::vector<SavedPosition>& list) {
    std::string s = "# Stranded Together saved positions: x y z name (edited by the in-game debug menu)\n";
    for (const SavedPosition& p : list) {
        s += FormatCoordinates(p.x, p.y, p.z);
        s += ' ';
        s += CleanName(p.name, "position");
        s += '\n';
    }
    return s;
}

bool ParseCoordinates(const char* text, double (&out)[3]) {
    if (!text) return false;
    const char* p = text;
    double v[3];
    for (double& d : v)
        if (!ReadNumber(p, d)) return false;
    while (*p && (IsSep(*p) || *p == '\n')) ++p;
    if (*p) return false;  // trailing junk
    out[0] = v[0], out[1] = v[1], out[2] = v[2];
    return true;
}

bool LoadPositions(const std::wstring& path, std::vector<SavedPosition>& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    std::string text;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (1 << 20);
    if (ok) {
        text.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        ok = text.empty() || (ReadFile(f, &text[0], static_cast<DWORD>(text.size()), &read, nullptr) && read == text.size());
    }
    CloseHandle(f);
    if (ok) out = ParsePositions(text);
    return ok;
}

bool SavePositions(const std::wstring& path, const std::vector<SavedPosition>& list) {
    const std::string text = FormatPositions(list);
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

}  // namespace st::overlay
