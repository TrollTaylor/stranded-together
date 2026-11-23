// Stranded Together textures: the launcher's "Extract textures" (docs/textures.md). Reads the user's own installed game
// read-only (<game>\data\*.bin through the game's oo2core) and writes a tidy folder of PNGs the user edits:
//   README.txt
//   Fragile porter suit\Original\             her maps (read-only, regenerated)
//   Fragile porter suit\NN <colour>\          one editable set per Sam suit colour (00 Default .. 16 Gold): her Colour,
//                                            Roughness and Reflectance in Stranded Together's ready-made colour (made
//                                            here from Original\ by suit_defaults.hpp; 00 Default: byte copies of her
//                                            own); Sam's colour.png
//   Fragile boots\NN <boot type>\             one editable boots set per boot type (her boots' layout) + Gold\ Silver\ Worn\
//   Sam porter suit\...                       every Sam suit style (reference, read-only), palette previews
//   Boots (Sam, reference)\...                every Sam boot type (reference, read-only)
//   .manifest.txt                            hidden index (tex_manifest.hpp)
// Re-run safe: an editable file is replaced only while it is still byte for byte what the extractor itself last wrote
// there (the manifest's seed row) and no picture of its folder was changed (a folder with any painted picture is the
// user's: never touched again); deleting one and extracting again resets it. Reference files are replaced one by one
// through "<name>.partial" files.
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
// Held (exclusively, deleted when closed) by a running extraction in the textures folder's top level: a second extraction
// into the same folder refuses, and the mod waits for its end before it reads the folder (a half-updated folder against
// the old index would look painted).
inline constexpr char kExtractLockName[] = ".extract.lock";
// The extractor's output version (the manifest's "extract" field): bumped whenever the pictures it writes or their names
// change (a decoder fix, a renamed map), so a re-extraction after a Stranded Together update rewrites the reference
// pictures instead of reusing them. 1: the first versioned output (round 6). 2: the colour folders 01..16 hold the
// ready-made colours (round 8; suit_defaults.hpp kGeneratorVersion 1): unedited v1 copies are replaced by them. 3: the
// polished gold and silver (round 9; kGeneratorVersion 2): unedited 15 Silver / 16 Gold pictures of a v2 extraction are
// replaced (01..14 are byte for byte the same as v2's).
inline constexpr int kExtractVersion = 3;
// The oldest extractor output version whose ready-made pictures of colour folder `colour` (1..16) are still this
// version's bytes: 2 for 01..14 (unchanged since round 8), 3 for 15 / 16; 0 for anything else (00 Default holds copies).
// A folder of an older extraction whose pictures are unedited shows an older look (Check my edits' "stale" count).
int ReadyMadeCurrentSince(int colour);

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
    // the ready-made colours (01..16): pictures created / replaced (an unedited older copy), folders left alone because
    // a picture there was painted, and the time spent making them ("defaults <written> <replaced> <yours> <ms>")
    int defaults_written = 0, defaults_replaced = 0, folders_yours = 0;
    std::uint64_t defaults_ms = 0;
    // unedited pictures this extraction could not bring up to date (another program had them open, or a picture could not
    // be made): the index then names an older extract version (incomplete_extract), so the next extraction, automatic
    // too, finishes them; "incomplete <n>" line
    int incomplete = 0;
    int incomplete_extract = 0;   // the extract version written instead of kExtractVersion (0: complete)
    int unchanged = 0;            // reference pictures already byte for byte what this extraction writes (not rewritten)
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
    int outdated_seeds = 0;              // unedited seeds of an older game version (00 Default and the boots folders)
    int ready_made = 0;                  // colour folders 01..16 with nothing painted: Fragile wears the ready-made colour
    int stale_ready_made = 0;            // ... of them, still showing an older extraction's pictures (Extract refreshes them)
    std::vector<std::string> problems;   // "16 Gold\Suit - Colour.png is 1024x1024: it will be resized to 2048x2048"...
    int colour_folders = 0, boot_folders = 0;
};
// decode: also decode every edited picture fully (Check my edits); otherwise only headers are read. Hashes only the
// pictures in the editable folders. COM must be initialised on the calling thread when decode is set.
void ScanEdits(const std::wstring& dir, bool decode, EditScan& out);
std::string EditSummary(const EditScan& s);   // "Edited: 16 Gold (Colour, Reflectance), 09 Chiral boots (Colour)" or ""
// ready_made: the launcher's READY-MADE COLOURS setting (Off: a folder the player did not paint is her own suit).
std::string CheckReport(const EditScan& s, bool ready_made = true);   // several lines for the launcher's Check my edits
// An extraction holds the folder's kExtractLockName right now.
bool ExtractionRunningIn(const std::wstring& dir);
// MoveFileEx(REPLACE_EXISTING), then a POSIX-semantics rename (succeeds while a reader that allows delete, like the mod,
// has the target open), retried for up to `wait_ms` while the target is held. False: the target is unchanged.
bool ReplaceFileRetrying(const std::wstring& from, const std::wstring& to, unsigned wait_ms = 3000);

// Path helpers (UTF-8 relative paths <-> wide absolute paths).
std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
std::wstring JoinPath(const std::wstring& dir, const std::string& rel_utf8);
// true when `path` is `root` or inside it (case-insensitive, after full-path normalisation).
bool PathInside(const std::wstring& path, const std::wstring& root);

}  // namespace st::tex
