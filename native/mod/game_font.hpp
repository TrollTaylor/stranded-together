// The game's own menu font, read at run time from the user's installed game (read-only, memory only).
//
// Name tags (overlay_tags.cpp) use the typeface of the game's menu headers ("Map/Information" when the
// map opens): interface/fonts/ds/expsmediumneon, a Decima FontResource made of vector outlines (not a
// TTF). docs/real-partner-body.md section 10.6. Nothing taken from the game is ever written to disk,
// shipped or logged: the outlines live in this process's memory only.
//
// The archive reader is a port of tools/re/ds_archive.py (packfile tables, murmur3 + MD5 decryption,
// the oo2core DLL from the game's own folder for Oodle). The game's files are opened for reading with full sharing.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace st::gamefont {

struct Contour {
    std::uint32_t first_cmd = 0, n_cmd = 0;  // into Font::cmds
    std::uint32_t first_pt = 0, n_pt = 0;    // into Font::pts (x, y pairs)
};

struct Glyph {
    std::uint32_t cp = 0;
    float advance = 0;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;  // of the outline points (font units, y up)
    std::uint32_t first_contour = 0, n_contours = 0;
};

// Font units: y up, baseline at 0. Commands per contour: b >> 6 = type, b & 63 = k.
// Type 0: k line-to points. Type 1: a TrueType-style quadratic B-spline of k points (k - 1
// off-curve controls with implied midpoints, then the on-curve end point). Points = 1 + sum of k.
struct Font {
    std::string typeface;
    float height = 0, ascent = 0, descent = 0, em_height = 0;
    std::vector<Glyph> glyphs;  // sorted by code point
    std::vector<Contour> contours;
    std::vector<std::uint8_t> cmds;
    std::vector<float> pts;
    std::size_t kerning_pairs = 0;  // counted, not used (ImGui's loader has no kerning)

    const Glyph* Find(std::uint32_t cp) const noexcept;
};

// The menu header font (what the game resolves interface/fonts/ds/exps_default to for every language).
constexpr const char* kMenuFontPath = "interface/fonts/ds/expsmediumneon.core";
// Its FontResource ObjectUUID 94f869a0-db23-3d46-a69a-c92036327756, as serialized (GUID bytes_le).
extern const std::uint8_t kMenuFontGuid[16];
constexpr std::uint64_t kFontResourceTypeId = 0xE0E68D6C7B7B08F3ull;

// Decima path hash (first u64 of MurmurHash3_x64_128, seed 42, over the lower-case path + NUL).
std::uint64_t PathHash(const char* path) noexcept;
void Murmur3x64_128(const void* data, std::size_t n, std::uint32_t seed, std::uint64_t out[2]) noexcept;

// Parses a core file holding one FontResource. Every count and size is validated; the parse must end
// exactly at the object's size and every contour must use exactly its points. guid: required
// ObjectUUID (nullptr: any).
bool ParseFontCore(const std::uint8_t* data, std::size_t n, const std::uint8_t* guid, Font* out, char* why, std::size_t cap) noexcept;

// Reads one core file from the game's data folder (<game_dir>\data\*.bin). preferred_archive (file
// name, may be null) is searched first, then every archive. Decompresses with OodleLZ_Decompress from an
// oo2core_*_win64.dll already loaded in this process, else (allow_load) the one in game_dir, loaded by full
// path for this call and freed again (ds.exe links Oodle statically and never loads that DLL itself).
bool ReadCore(const std::wstring& game_dir, const char* path, const wchar_t* preferred_archive, bool allow_load,
              std::vector<std::uint8_t>* out, char* why, std::size_t cap) noexcept;

// ReadCore + ParseFontCore for the menu font.
bool LoadMenuFont(const std::wstring& game_dir, bool allow_load, Font* out, char* why, std::size_t cap) noexcept;

}  // namespace st::gamefont
