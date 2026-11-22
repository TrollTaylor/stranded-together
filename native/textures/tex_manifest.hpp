// Stranded Together textures: <Textures>\.manifest.txt, the extracted folder's index (UTF-8, tab-separated, hidden).
//   stx-manifest 1
//   table <kTableVersion hex>	game <ds.exe size>-<ds.exe mtime>	tool <launcher version>	extract <kExtractVersion>
//   (the extract field since round 6; a manifest without it reads as 0)
//   file <relative path>	<slot id|ref>	<component RGB|RG|R|A|...>	<w>x<h>	<sha256 hex>	<kind>	<bytes>
// kind: original (Fragile's own maps, read-only), seed (an editable copy; its sha is the hash of the bytes the
// extractor first wrote, kept across re-runs while the file is unedited), ref (Sam's reference maps, read-only),
// preview (generated pictures: palette previews, swatches). The mod reads only this file: it refuses another version
// or a table value other than its own.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace st::tex {

inline constexpr char kManifestName[] = ".manifest.txt";
inline constexpr int kManifestVersion = 1;

struct ManifestRow {
    std::string path;        // relative to the textures folder, backslashes
    std::string slot;        // "S0".."B2" or "ref"
    std::string component;   // which channels of the slot's texture this picture fills
    std::uint32_t w = 0, h = 0;
    std::string sha256;      // lowercase hex
    std::string kind;        // original | seed | ref | preview
    std::uint64_t bytes = 0; // file size when written
};

struct Manifest {
    std::uint32_t table = 0;
    std::string game;        // "<size>-<mtime>" of ds.exe
    std::string tool;        // the launcher version that wrote it
    int extract = 0;         // the extractor's output version (extract_plan.hpp kExtractVersion; 0 = before it existed)
    std::vector<ManifestRow> rows;
    const ManifestRow* Find(const std::string& path) const;   // case-insensitive path match
};

std::string WriteManifest(const Manifest& m);
// False + a reason for another version, a malformed line or an unknown kind.
bool ParseManifest(const std::string& text, Manifest& out, std::string& error);

// Case-insensitive compare of two relative paths ('/' and '\' equal).
bool SamePath(const std::string& a, const std::string& b);

// Folder names that start with a two-digit number and a space ("16 Gold", "09 Chiral boots") -> that number;
// -1 for anything else. The mod matches folders by this prefix, so the user may rename the rest.
int FolderNumber(const std::string& name);

}  // namespace st::tex
