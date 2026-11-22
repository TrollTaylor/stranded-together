// Stranded Together textures: the launcher's "Extract textures" (docs/textures.md). Reads the user's own installed game
// read-only (<game>\data\*.bin through the game's oo2core) and writes a tidy folder of PNGs the user edits:
//   README.txt
//   Fragile porter suit\Original\             her maps (read-only, regenerated)
//   Fragile porter suit\NN <colour>\          one editable set per Sam suit colour (00 Default .. 16 Gold), seeded as
//                                            byte copies of her Colour, Roughness and Reflectance; Sam's colour.png
//   Fragile boots\NN <boot type>\             one editable boots set per boot type (her boots' layout) + Gold\ Silver\ Worn\
//   Sam porter suit\...                       every Sam suit style (reference, read-only), palette previews
//   Boots (Sam, reference)\...                every Sam boot type (reference, read-only)
//   .manifest.txt                            hidden index (tex_manifest.hpp)
// Re-run safe: an editable file is only ever created (never opened for writing when it exists); deleting one and
// extracting again resets it. Reference files are replaced one by one through "<name>.partial" files.
// Only for the launcher's child process (--extract-textures) and the tests: never linked into the mod.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "tex_manifest.hpp"

namespace st::tex {

inline constexpr char kFragileSuitDir[] = "Fragile porter suit";
inline constexpr char kOriginalDir[] = "Fragile porter suit\\Original";
inline constexpr char kFragileBootsDir[] = "Fragile boots";
inline constexpr char kSamSuitDir[] = "Sam porter suit";
inline constexpr char kSamBootsDir[] = "Boots (Sam, reference)";
inline constexpr char kReadmeName[] = "README.txt";
// The extractor's output version (the manifest's "extract" field): bumped whenever the pictures it writes or their names
// change (a decoder fix, a renamed map), so a re-extraction after a Stranded Together update rewrites the reference
// pictures instead of reusing them. 1: the first versioned output (round 6).
inline constexpr int kExtractVersion = 1;

struct NamedFolder {
    int number;          // the folder's NN prefix: Sam's suit colour (0..16) or EDSPlayerShoesType (0, 2..10)
    const char* name;    // "16 Gold", "09 Chiral boots"
};
extern const NamedFolder kColourFolders[17];
extern const NamedFolder kBootFolders[10];
const char* ColourName(int colour);   // "Gold" ("" outside 0..16)

// One TextureSet to extract: every map of it goes to <folder>\<part> - <Map>.png.
struct ExtractJob {
    const char* folder;
    const char* part;
    const char* core_path;
    bool original;       // Fragile's own maps (kind original); otherwise Sam's reference (kind ref)
};
const std::vector<ExtractJob>& ExtractJobs();
// The editable seeds: copies of these Original files.
const std::vector<std::string>& SuitSeedNames();   // Suit - Colour.png, Suit - Roughness.png, Suit - Reflectance.png
const std::vector<std::string>& BootSeedNames();   // Boots - Colour.png, Boots - Roughness.png, Boots - Reflectance.png
// Pictures the loader also takes from an editable folder when present (copied in by the user from Original).
bool IsUsableSuitName(const std::string& file);
bool IsUsableBootName(const std::string& file);

struct ExtractOptions {
    std::wstring game_dir;
    std::wstring out_dir;
    bool previews = true;               // Sam palette previews (about 15 MB)
    int cancel_after_sets = -1;         // tests: the process ends itself in the middle of set N (a cancelled run)
    std::string tool_version = "dev";
    std::uint64_t min_free_bytes = 400ull << 20;
    std::function<void(const std::string& line)> emit;   // progress / skip / kept / done lines
    // Tests only: a decompressor instead of the game's oo2core, and another job list (a synthetic archive).
    std::function<bool(const std::uint8_t*, std::size_t, std::uint8_t*, std::size_t)> decompressor;
    const std::vector<ExtractJob>* jobs = nullptr;
    // Fragile's own sets must match the slot table (tex_slots.hpp): every slot found with its size, mips, format, sRGB,
    // content GUID, views and packing, else the extraction fails with a sentence. Tests with a synthetic archive: false.
    bool check_slots = true;
};
struct ExtractStats {
    int sets_done = 0, sets_skipped = 0, sets_reused = 0;
    int written = 0, kept = 0;
    int outdated_seeds = 0;
    std::uint64_t bytes = 0;
    std::uint64_t ms = 0;
};
// Folders and files are created only under o.out_dir. False + a sentence for the player.
bool RunExtract(const ExtractOptions& o, ExtractStats& stats, std::string& error);

// ds.exe's size and last-write time as "<size>-<mtime>" ("unknown" without ds.exe).
std::string GameStamp(const std::wstring& game_dir);
std::string ReadmeText();
std::uint64_t EstimatedBytes(bool previews);   // shown before extraction

// ---- Status and "Check my edits" ----------------------------------------------------------------------------
struct EditedFolder {
    std::string folder;                  // relative: "Fragile porter suit\16 Gold", "Fragile boots\09 Chiral boots\Worn"
    std::vector<std::string> maps;       // "Colour", "Reflectance"...
};
struct EditScan {
    bool extracted = false;              // a valid manifest exists
    std::string manifest_error;          // why not
    Manifest manifest;
    bool table_ok = false;               // the manifest's table matches this build's slot table
    std::string extracted_utc;           // the manifest's write time "YYYY-MM-DD HH:MM" (local)
    std::vector<EditedFolder> edited;    // folders with at least one edited picture
    int outdated_seeds = 0;              // unedited seeds of an older game version
    std::vector<std::string> problems;   // "16 Gold\Suit - Colour.png is 1024x1024: it will be resized to 2048x2048"...
    int colour_folders = 0, boot_folders = 0;
};
// decode: also decode every edited picture fully (Check my edits); otherwise only headers are read. Hashes only the
// pictures in the editable folders. COM must be initialised on the calling thread when decode is set.
void ScanEdits(const std::wstring& dir, bool decode, EditScan& out);
std::string EditSummary(const EditScan& s);   // "Edited: 16 Gold (Colour, Reflectance), 09 Chiral boots (Colour)" or ""
std::string CheckReport(const EditScan& s);   // several lines for the launcher's Check my edits

// Path helpers (UTF-8 relative paths <-> wide absolute paths).
std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
std::wstring JoinPath(const std::wstring& dir, const std::string& rel_utf8);
// true when `path` is `root` or inside it (case-insensitive, after full-path normalisation).
bool PathInside(const std::wstring& path, const std::wstring& root);

}  // namespace st::tex
