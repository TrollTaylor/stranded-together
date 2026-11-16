// Saved teleport positions: <game>\stranded_together\positions.txt.
//
// One position per line, coordinates first so names may contain spaces:
//     # comment
//     1234.500 56.250 -789.125 Capital Knot City gate
// Parsing is tolerant (commas, tabs, parentheses; bad lines skipped). Pure
// except Load/Save (plain file I/O on the render thread, never game memory).
#pragma once
#include <string>
#include <vector>

namespace st::overlay {

struct SavedPosition {
    std::string name;
    double x = 0, y = 0, z = 0;
};

std::vector<SavedPosition> ParsePositions(const std::string& text);
std::string FormatPositions(const std::vector<SavedPosition>& list);
// "x y z" with millimetre precision (what "Copy position" puts on the clipboard).
std::string FormatCoordinates(double x, double y, double z);
// Accepts "x y z", "x, y, z", "(x, y, z)", "x;y;z"; false unless exactly three finite numbers lead.
bool ParseCoordinates(const char* text, double (&out)[3]);
// Name sanitising: control characters/newlines removed, trimmed, capped at 64 bytes; empty -> fallback.
std::string CleanName(const std::string& name, const std::string& fallback);

bool LoadPositions(const std::wstring& path, std::vector<SavedPosition>& out);
// Atomic replace (tmp + MoveFileEx).
bool SavePositions(const std::wstring& path, const std::vector<SavedPosition>& list);

}  // namespace st::overlay
