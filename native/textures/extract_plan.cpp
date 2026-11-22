// Stranded Together textures: "Extract textures" (extract_plan.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "decima_archive.hpp"
#include "decima_core.hpp"
#include "extract_plan.hpp"
#include "oodle_dyn.hpp"
#include "png_wic.hpp"
#include "suit_colour_math.hpp"
#include "tex_codec.hpp"
#include "tex_hash.hpp"
#include "tex_slots.hpp"

namespace st::tex {

// ---- Names ---------------------------------------------------------------------------------------------------
// Sam's suit colours: the uniform menu's order (inventory +0x4BE1); 0..14 named after cm_cvar_b002's column 0
// (suit_colour_math.hpp kLut), 15 / 16 the silver and gold sets. The game's menu shows no names.
const NamedFolder kColourFolders[17] = {{0, "00 Default"}, {1, "01 Orange"},     {2, "02 Red"},    {3, "03 Charcoal"}, {4, "04 Sand"},
                                        {5, "05 Light grey"}, {6, "06 Brown"},   {7, "07 Lavender"}, {8, "08 Plum"},   {9, "09 Mustard"},
                                        {10, "10 Brick"},  {11, "11 Grey"},      {12, "12 Olive"}, {13, "13 Sky blue"}, {14, "14 Pink"},
                                        {15, "15 Silver"}, {16, "16 Gold"}};
// EDSPlayerShoesType in the enum's string order (1, ShoesA_Unused, has no folder: it uses 03 Porter boots).
const NamedFolder kBootFolders[10] = {{0, "00 No boots (barefoot)"}, {2, "02 Sole grass"},           {3, "03 Porter boots"},
                                      {4, "04 All-terrain boots Lv1"}, {5, "05 All-terrain boots Lv2"}, {6, "06 All-terrain boots Lv3"},
                                      {7, "07 Mule boots"},          {8, "08 Terrorist boots"},       {9, "09 Chiral boots"},
                                      {10, "10 Reflect boots"}};

const char* ColourName(int c) {
    if (c < 0 || c > 16) return "";
    return kColourFolders[c].name + 3;
}

namespace {
constexpr char kF[] = "ds/models/characters/fgl_fragile/core/fgl_textures/textures/";
constexpr char kS[] = "ds/models/characters/sam_sam/core/sam_textures/textures/";
std::string Cat(const char* a, const char* b) { return std::string(a) + b; }
}  // namespace

const std::vector<ExtractJob>& ExtractJobs() {
    static const std::vector<std::string> paths = {
        Cat(kF, "fgl_body_suiti_v00_set"), Cat(kF, "fgl_part_suiti_v00_set"), Cat(kF, "fgl_part_hand_dmo_set"), Cat(kF, "fgl_part_hand_shrd_set"),
        "ds/models/characters/dhm_diehardman/core/dhm_textures/textures/dhm_budge_def_v00_set",
        "ds/models/characters/bgm_bridgesmember/core/bgm_textures/textures/bgm_body_neck_v00", Cat(kS, "sam_body_boot_v00_set"),
        // Sam's porter suit
        Cat(kS, "sam_body_suiti_v00_set"), Cat(kS, "sam_parts_suiti_v00_set"), Cat(kS, "sam_glove_suiti_v00_set"), Cat(kS, "sam_body_subpouch_v00_set"),
        Cat(kS, "sam_body_suiti_emt_v00_set"), Cat(kS, "sam_body_seaotterhood_v00_set"), Cat(kS, "sam_glove_power_set"),
        Cat(kS, "sam_body_suiti_v02_set"), Cat(kS, "sam_body_seaotterhood_v02_set"), Cat(kS, "sam_glove_power_v01_set"),
        Cat(kS, "sam_body_suiti_v03_set"), Cat(kS, "sam_body_seaotterhood_v03_set"), Cat(kS, "sam_glove_power_v02_set"),
        Cat(kS, "sam_body_suiti_v01_set"), Cat(kS, "sam_parts_suiti_v01_set"),
        // Sam's boots
        Cat(kS, "sam_body_boot_v00_set"), Cat(kS, "sam_body_boot_v01_set"), Cat(kS, "sam_boot_mk2_v00_set"), Cat(kS, "sam_boot_mk3_v00_set"),
        Cat(kS, "sam_boot_mk3_v01_set"), Cat(kS, "sam_boot_mk3_v02_set"), Cat(kS, "sam_boots_suitmule_v00_set"),
        "ds/models/characters/mut_muleterrorist/core/mut_textures/textures/mut_boot_def_v00_set", Cat(kS, "sam_boot_bandage_v00_set")};
    static const std::vector<ExtractJob> jobs = [] {
        const char* o = kOriginalDir;
        const char* pal = "Sam porter suit\\Palette colours (00-14)";
        const char* sil = "Sam porter suit\\15 Silver";
        const char* gold = "Sam porter suit\\16 Gold";
        const char* dark = "Sam porter suit\\Dark (cutscene only)";
        const char* b = "Boots (Sam, reference)\\";
        static const std::vector<std::string> bootDirs = {
            std::string(b) + "Porter boots", std::string(b) + "Porter boots v01", std::string(b) + "Mk2 boots",
            std::string(b) + "Mk3 boots",    std::string(b) + "Mk3 boots gold",   std::string(b) + "Mk3 boots silver",
            std::string(b) + "Mule boots",   std::string(b) + "Terrorist boots",  std::string(b) + "Bandaged feet"};
        std::vector<ExtractJob> j = {
            {o, "Suit", paths[0].c_str(), true},         {o, "Suit parts", paths[1].c_str(), true}, {o, "Gloves", paths[2].c_str(), true},
            {o, "Gloves detail", paths[3].c_str(), true}, {o, "Badge", paths[4].c_str(), true},      {o, "Neck", paths[5].c_str(), true},
            {o, "Boots", paths[6].c_str(), true},
            {pal, "Suit", paths[7].c_str(), false},      {pal, "Suit parts", paths[8].c_str(), false}, {pal, "Gloves", paths[9].c_str(), false},
            {pal, "Pouch", paths[10].c_str(), false},    {pal, "Lamps", paths[11].c_str(), false},
            {pal, "Hood (sea otter)", paths[12].c_str(), false}, {pal, "Power glove", paths[13].c_str(), false},
            {sil, "Suit", paths[14].c_str(), false},     {sil, "Hood (sea otter)", paths[15].c_str(), false}, {sil, "Power glove", paths[16].c_str(), false},
            {gold, "Suit", paths[17].c_str(), false},    {gold, "Hood (sea otter)", paths[18].c_str(), false}, {gold, "Power glove", paths[19].c_str(), false},
            {dark, "Suit", paths[20].c_str(), false},    {dark, "Suit parts", paths[21].c_str(), false},
        };
        for (std::size_t i = 0; i < bootDirs.size(); ++i)
            j.push_back({bootDirs[i].c_str(), i + 1 == bootDirs.size() ? "Feet" : "Boots", paths[22 + i].c_str(), false});
        return j;
    }();
    return jobs;
}

const std::vector<std::string>& SuitSeedNames() {
    static const std::vector<std::string> n = {"Suit - Colour.png", "Suit - Roughness.png", "Suit - Reflectance.png"};
    return n;
}
const std::vector<std::string>& BootSeedNames() {
    static const std::vector<std::string> n = {"Boots - Colour.png", "Boots - Roughness.png", "Boots - Reflectance.png"};
    return n;
}
bool IsUsableSuitName(const std::string& f) {
    for (const char* n : {"Suit - Colour.png", "Suit - Roughness.png", "Suit - Reflectance.png", "Suit - Ambient occlusion.png",
                          "Suit - Normal.png", "Suit - Mask.png"})
        if (SamePath(f, n)) return true;
    return false;
}
bool IsUsableBootName(const std::string& f) {
    for (const char* n : {"Boots - Colour.png", "Boots - Roughness.png", "Boots - Reflectance.png", "Boots - Ambient occlusion.png",
                          "Boots - Normal.png"})
        if (SamePath(f, n)) return true;
    return false;
}

// ---- Paths ---------------------------------------------------------------------------------------------------
std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring JoinPath(const std::wstring& dir, const std::string& rel) {
    std::wstring r = Widen(rel);
    for (wchar_t& c : r)
        if (c == L'/') c = L'\\';
    if (dir.empty()) return r;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + r;
    return dir + L"\\" + r;
}
namespace {
std::wstring FullLower(const std::wstring& p) {
    wchar_t buf[4096];
    const DWORD n = GetFullPathNameW(p.c_str(), 4096, buf, nullptr);
    std::wstring s = (n > 0 && n < 4096) ? std::wstring(buf, n) : p;
    for (wchar_t& c : s) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}
}  // namespace
bool PathInside(const std::wstring& path, const std::wstring& root) {
    const std::wstring p = FullLower(path), r = FullLower(root);
    if (p == r) return true;
    return p.size() > r.size() && p.compare(0, r.size(), r) == 0 && (p[r.size()] == L'\\' || r.back() == L'\\');
}

std::string GameStamp(const std::wstring& game_dir) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(JoinPath(game_dir, "ds.exe").c_str(), GetFileExInfoStandard, &a)) return "unknown";
    const std::uint64_t size = (static_cast<std::uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    const std::uint64_t t = (static_cast<std::uint64_t>(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return std::to_string(size) + "-" + std::to_string(t);
}

std::uint64_t EstimatedBytes(bool previews) { return (previews ? 330ull : 315ull) << 20; }

// ---- File writing ----------------------------------------------------------------------------------------------
namespace {

bool IsDir(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool IsFile(const std::wstring& p, std::uint64_t* size = nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (size) *size = (static_cast<std::uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}
bool MakeDirs(const std::wstring& p) {
    if (p.empty() || IsDir(p)) return true;
    const std::size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2 && !MakeDirs(p.substr(0, slash))) return false;
    return CreateDirectoryW(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
bool WriteWhole(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = bytes.empty() || (WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size());
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
bool ReadWhole(const std::wstring& path, std::vector<std::uint8_t>& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart < (512ll << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}
bool EndsWithPartialPng(const std::wstring& n) {
    // what WritePartial leaves: "<name>.png.partial" (pictures), "<name>.txt.partial" (README, the index)
    if (n.size() <= 12 || _wcsicmp(n.c_str() + n.size() - 8, L".partial") != 0) return false;
    const wchar_t* ext = n.c_str() + n.size() - 12;
    return _wcsnicmp(ext, L".png", 4) == 0 || _wcsnicmp(ext, L".txt", 4) == 0;
}
void DeletePartials(const std::wstring& dir, int depth) {
    if (depth > 4) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // never follow links out of the folder
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DeletePartials(p, depth + 1);
        } else if (EndsWithPartialPng(n)) {
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(p.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
// Every entry of a folder (files and folders, links included), "." and ".." left out.
std::vector<std::wstring> ListAll(const std::wstring& dir) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
// The names the extraction itself puts at the top of the textures folder (a cancelled first run leaves only these).
bool IsOurTopName(const std::wstring& n) {
    for (const char* ours : {kFragileSuitDir, kFragileBootsDir, kSamSuitDir, kSamBootsDir, kReadmeName, kManifestName, ".cache"})
        if (_wcsicmp(n.c_str(), Widen(ours).c_str()) == 0 || _wcsicmp(n.c_str(), (Widen(ours) + L".partial").c_str()) == 0) return true;
    return false;
}
// A cancelled run's leftovers: only in our own folders (and our two files at the top), never anywhere else the folder holds.
void DeleteOurPartials(const std::wstring& out_dir) {
    for (const char* f : {kReadmeName, kManifestName}) {
        const std::wstring p = out_dir + L"\\" + Widen(f) + L".partial";
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
    }
    for (const char* d : {kFragileSuitDir, kFragileBootsDir, kSamSuitDir, kSamBootsDir}) {
        const std::wstring p = out_dir + L"\\" + Widen(d);
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) DeletePartials(p, 0);
    }
}
std::vector<std::wstring> ListDir(const std::wstring& dir, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        const bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (d == dirs && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}
std::string ShaHex(const std::vector<std::uint8_t>& bytes) {
    Sha256 d{};
    HashSha256(bytes.data(), bytes.size(), d);
    return Hex(d);
}
std::string FileShaHex(const std::wstring& path) {
    Sha256 d{};
    return HashFileSha256(path, d) ? Hex(d) : std::string();
}

class Out {
public:
    Out(const ExtractOptions& o, ExtractStats& s) : o_(o), s_(s) {}
    std::wstring Abs(const std::string& rel) const { return JoinPath(o_.out_dir, rel); }
    void Emit(const std::string& line) const {
        if (o_.emit) o_.emit(line);
    }
    // The cancel hook of the tests: ends the process with a .partial on disk.
    bool WritePartial(const std::wstring& abs, const std::vector<std::uint8_t>& bytes) {
        if (!WriteWhole(abs + L".partial", bytes)) return false;
        if (cancel_armed_) {
            Emit("cancelled (test hook) after a partial file");
            TerminateProcess(GetCurrentProcess(), 3);
        }
        return true;
    }
    // A read-only reference file, replaced in one step.
    bool Reference(const std::string& rel, const std::vector<std::uint8_t>& bytes, std::string& error) {
        const std::wstring abs = Abs(rel);
        const std::size_t slash = abs.find_last_of(L'\\');
        if (!MakeDirs(abs.substr(0, slash))) return error = "cannot create the folder for " + rel, false;
        if (!WritePartial(abs, bytes)) return error = "cannot write " + rel + " (is the disk full?)", false;
        SetFileAttributesW(abs.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW((abs + L".partial").c_str(), abs.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW((abs + L".partial").c_str());
            return error = "cannot replace " + rel + " (is it open in another program?)", false;
        }
        SetFileAttributesW(abs.c_str(), FILE_ATTRIBUTE_READONLY);
        ++s_.written;
        s_.bytes += bytes.size();
        return true;
    }
    // An editable seed: created only when missing (1), kept when it exists (0), -1 on an error.
    int Seed(const std::string& rel, const std::vector<std::uint8_t>& bytes, std::string& error) {
        const std::wstring abs = Abs(rel);
        if (IsFile(abs)) return 0;
        const std::size_t slash = abs.find_last_of(L'\\');
        if (!MakeDirs(abs.substr(0, slash))) return error = "cannot create the folder for " + rel, -1;
        if (!WritePartial(abs, bytes)) return error = "cannot write " + rel, -1;
        // No REPLACE_EXISTING: a file that appeared meanwhile is never overwritten.
        if (!MoveFileExW((abs + L".partial").c_str(), abs.c_str(), 0)) {
            DeleteFileW((abs + L".partial").c_str());
            return 0;
        }
        ++s_.written;
        s_.bytes += bytes.size();
        return 1;
    }
    bool cancel_armed_ = false;

private:
    const ExtractOptions& o_;
    ExtractStats& s_;
};

// One map picture of a texture.
struct MapOut {
    std::string rel;
    std::string slot;
    std::string component;
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> png;
};

// 3x5 digits for the swatch labels.
constexpr std::uint16_t kDigits[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};
void DrawNumber(Image& img, int number, std::uint32_t x0, std::uint32_t y0, std::uint32_t scale, const std::uint8_t ink[3]) {
    const int digits[2] = {number / 10 % 10, number % 10};
    for (int d = 0; d < 2; ++d) {
        const std::uint16_t g = kDigits[digits[d]];
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col) {
                if (!((g >> (14 - (row * 3 + col))) & 1)) continue;
                for (std::uint32_t yy = 0; yy < scale; ++yy)
                    for (std::uint32_t xx = 0; xx < scale; ++xx) {
                        const std::uint32_t x = x0 + static_cast<std::uint32_t>(d * 4 + col) * scale + xx, y = y0 + static_cast<std::uint32_t>(row) * scale + yy;
                        if (x < img.w && y < img.h) std::memcpy(img.At(x, y), ink, 3);
                    }
            }
    }
}
void Fill(Image& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, const std::uint8_t rgb[3]) {
    for (std::uint32_t y = y0; y < y0 + h && y < img.h; ++y)
        for (std::uint32_t x = x0; x < x0 + w && x < img.w; ++x) {
            std::uint8_t* p = img.At(x, y);
            p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 255;
        }
}
void SwatchColour(int c, std::uint8_t rgb[3]) {
    if (c >= 0 && c < 15) {
        rgb[0] = suitcol::kLut[c].r, rgb[1] = suitcol::kLut[c].g, rgb[2] = suitcol::kLut[c].b;
    } else {
        const suitcol::Rgb m = c == 15 ? suitcol::kSilverF0 : suitcol::kGoldF0;   // the metal's reflectance colour
        rgb[0] = m.r, rgb[1] = m.g, rgb[2] = m.b;
    }
}

}  // namespace

// ---- README ----------------------------------------------------------------------------------------------------
std::string ReadmeText() {
    return
        "Stranded Together - Fragile's textures\r\n"
        "======================================\r\n"
        "\r\n"
        "These pictures come from YOUR copy of DEATH STRANDING DIRECTOR'S CUT (the launcher's \"Extract textures\").\r\n"
        "They are for your own use: do not share or distribute them. The game's files are never changed.\r\n"
        "\r\n"
        "WHERE TO EDIT\r\n"
        "  Fragile porter suit\\NN <colour>\\   one folder per Sam suit colour (the uniform colour the guest's Sam wears).\r\n"
        "                                     00 Default .. 14 Pink are the 15 palette colours, 15 Silver and 16 Gold the metal suits.\r\n"
        "                                     Fragile shows the folder that matches the guest's Sam suit colour.\r\n"
        "  Fragile boots\\NN <boot type>\\      one folder per boot type the guest has equipped (03 Porter boots, 09 Chiral boots...).\r\n"
        "                                     Gold\\, Silver\\ and Worn\\ inside are used when they hold an edited picture:\r\n"
        "                                     Worn\\ while the boots are worn out, Gold\\ / Silver\\ with the gold / silver suit.\r\n"
        "  The two-digit number at the start of a folder name is what counts: you may rename the rest.\r\n"
        "\r\n"
        "Every editable folder starts as an exact copy of Fragile's own pictures, so nothing changes until you paint.\r\n"
        "Only pictures you changed are used; an untouched folder means her normal look.\r\n"
        "\r\n"
        "WHAT EACH PICTURE DOES\r\n"
        "  Colour             the paint (albedo): the fabric colour, the prints, the stripes. sRGB.\r\n"
        "  Roughness          grey: black = glossy / shiny, white = matte.\r\n"
        "  Reflectance        the colour of the shine (specular colour). Cloth is dark grey here. Gold and silver live in\r\n"
        "                     this picture: a dark Colour, a bright tinted Reflectance and a low (dark) Roughness make metal.\r\n"
        "  Ambient occlusion  grey: baked shadow in folds and seams (white = no shadow).\r\n"
        "  Normal             the small bumps (seams, fabric weave): red/green = the tilt. Edit with care.\r\n"
        "  Mask               material regions for the game's detail layers.\r\n"
        "  Normal, Ambient occlusion and Mask are not copied into the colour folders: copy one in from Original\\ (same\r\n"
        "  name) if you want to change it.\r\n"
        "\r\n"
        "SIZES AND FORMAT\r\n"
        "  Keep each picture's size: the suit's Colour, Roughness and Normal are 2048 x 2048, its Reflectance, Ambient\r\n"
        "  occlusion and Mask 1024 x 1024; the boots' pictures are 1024 x 1024 (see Original\\). A picture of another\r\n"
        "  size is resized (\"Check my edits\" in the launcher tells you). Save as PNG with the same file name.\r\n"
        "  Transparency is ignored.\r\n"
        "\r\n"
        "REFERENCE FOLDERS (read-only, regenerated by every extraction)\r\n"
        "  Fragile porter suit\\Original\\     her own pictures, also her gloves, neck, badge and suit parts.\r\n"
        "  Sam porter suit\\                  every Sam suit style: Palette colours (00-14) (one light grey picture that\r\n"
        "                                     the game tints; Previews\\ shows each colour, roughly), 15 Silver, 16 Gold and\r\n"
        "                                     Dark (cutscene only). Colour swatches.png shows the 15 palette colours.\r\n"
        "  Boots (Sam, reference)\\           every Sam boot type.\r\n"
        "  Sam's suit and boots are laid out differently from Fragile's: copy colours and small details, not whole\r\n"
        "  pictures. Fragile's boots always keep the shape of her porter boots: only their look changes.\r\n"
        "  The sea otter hood's normal maps are in a format the extractor does not decode, so they are missing.\r\n"
        "\r\n"
        "RESETTING\r\n"
        "  Your folders are never overwritten. To reset one picture, delete it and press \"Extract textures\" again: it\r\n"
        "  comes back as Fragile's original. Original\\ and Sam's folders are rebuilt by every extraction.\r\n"
        "  This README.txt is rewritten by every extraction too: keep your own notes in a file of your own.\r\n"
        "\r\n"
        "IN THE GAME\r\n"
        "  The guest's Fragile wears the edited pictures on both screens (yours and your partner's). Changes show a few\r\n"
        "  seconds after you save; your partner receives them over the link (about 2 minutes for a full set the first\r\n"
        "  time, then they are cached). Sam is never changed. Only the guest's Fragile uses this folder; the host's\r\n"
        "  game shows what the guest sends.\r\n"
        "  Her boots: by default (launcher: \"Her own boots, painted with your boot pictures\") she wears her own porter\r\n"
        "  boots on both screens, painted with the pictures of the folder of the boot type the guest's Sam has on\r\n"
        "  (Fragile boots\\NN <boot type>\\). Their shape is always her porter boot; worn-out boots show Worn\\ if you\r\n"
        "  painted one. The other choice (\"Sam's boots on her feet\") draws Sam's real boots of that type over her feet\r\n"
        "  instead, and these pictures are then hidden under them.\r\n";
}

// ---- Extraction ------------------------------------------------------------------------------------------------
namespace {

struct LoadedSet {
    TextureSetInfo set;
    std::map<std::string, TextureInfo> textures;   // by ObjectUUID
};

bool LoadSet(ArchiveSet& arc, const Decompressor& dec, const std::string& core_path, LoadedSet& out, std::string& why) {
    std::vector<std::uint8_t> core;
    std::string err;
    if (!arc.ReadFile(CorePathHash(core_path), dec, core, err)) return why = "core: " + err, false;
    std::vector<CoreObject> objs;
    if (!ReadCoreObjects(core, objs, err)) return why = err, false;
    bool have_set = false;
    for (const CoreObject& o : objs) {
        if (o.type == kTypeTexture) {
            TextureInfo t;
            if (!ParseTexture(o.body, o.size, t, err)) return why = "texture: " + err, false;
            out.textures[t.guid] = std::move(t);
        } else if (o.type == kTypeTextureSet && !have_set) {
            if (!ParseTextureSet(o.body, o.size, out.set, err)) return why = "texture set: " + err, false;
            have_set = true;
        }
    }
    if (!have_set) return why = "no TextureSet in the core file", false;
    return true;
}

// How a texture of Fragile's own sets differs from its slot row ("" = it matches): the mod replaces exactly these.
std::string SlotMismatch(const Slot& s, const TextureInfo& t, const TextureSetEntry& e, const std::string& set_guid) {
    char b[200];
    if (set_guid != s.set_guid) return std::string(s.id) + ": its texture set is " + set_guid + ", not " + s.set_guid;
    if (t.width != s.w || t.height != s.h) {
        std::snprintf(b, sizeof(b), "%s: %ux%u, not %ux%u", s.id, t.width, t.height, s.w, s.h);
        return b;
    }
    if (t.mips != s.mips || t.format != s.orig_fmt || t.srgb != s.srgb) {
        std::snprintf(b, sizeof(b), "%s: %u mips, format %u, sRGB %u (not %u, %u, %u)", s.id, t.mips, t.format, t.srgb, s.mips, s.orig_fmt, s.srgb);
        return b;
    }
    if (t.content_guid != s.content_guid) return std::string(s.id) + ": its content GUID is " + t.content_guid;
    if (t.views != s.views || e.packing != s.packing) {
        std::snprintf(b, sizeof(b), "%s: views %llx, packing %08x (not %llx, %08x)", s.id, static_cast<unsigned long long>(t.views), e.packing,
                      static_cast<unsigned long long>(s.views), s.packing);
        return b;
    }
    return {};
}

// Every map of one set -> PNGs in memory. A map that cannot be decoded is skipped with a line. `found` (Fragile's own sets,
// options.check_slots): the slots matched, checked against their rows; a row that does not match fails (`game_changed`).
bool BuildMaps(ArchiveSet& arc, const Decompressor& dec, const ExtractJob& job, const LoadedSet& ls, std::vector<MapOut>& maps,
               std::vector<std::string>& skips, Image* keep_colour, std::set<std::string>* found, bool& game_changed, std::string& why) {
    std::set<std::string> used;
    for (const TextureSetEntry& e : ls.set.entries) {
        if (e.texture_guid.empty()) continue;
        auto it = ls.textures.find(e.texture_guid);
        if (it == ls.textures.end()) {
            skips.push_back(std::string(job.part) + ": a texture outside the set's core file");
            continue;
        }
        const TextureInfo& t = it->second;
        if (found && job.original) {
            if (const Slot* s = SlotByObjectGuid(t.guid.c_str())) {
                const std::string bad = SlotMismatch(*s, t, e, ls.set.guid);
                if (!bad.empty()) {
                    game_changed = true;
                    return why = bad, false;
                }
                found->insert(s->id);
            }
        }
        if (t.width <= 4 && t.height <= 4) continue;   // 1x1 constants
        std::vector<std::uint8_t> stream;
        std::string err;
        if (t.stream_size) {
            if (!arc.ReadFile(CorePathHash(t.stream_path + ".core.stream"), dec, stream, err)) return why = "stream: " + err, false;
        }
        if (!CheckTexture(t, stream.size(), err)) return why = t.guid + ": " + err, false;
        const std::vector<PackedMap> packed = MapsOf(e.packing);
        if (!CanDecode(t.format)) {
            for (const PackedMap& m : packed)
                skips.push_back(std::string(job.part) + " - " + MapFileName(m.type) + ": " + FormatName(t.format) + " is not decoded");
            continue;
        }
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;
        if (!MipData(t, stream, 0, data, size, err)) return why = t.guid + ": " + err, false;
        Image img;
        if (!Decode(t.format, data, size, t.width, t.height, img, &err)) return why = t.guid + ": " + err, false;
        const Slot* slot = job.original ? SlotByObjectGuid(t.guid.c_str()) : nullptr;
        for (const PackedMap& m : packed) {
            if (m.type == kTypeMisc01 && t.width <= 4) continue;
            std::string base = std::string(job.part) + " - " + MapFileName(m.type);
            std::string name = base + ".png";
            for (int n = 2; used.count(name); ++n) name = base + " " + std::to_string(n) + ".png";
            used.insert(name);
            Image pic;
            PngLayout layout;
            if (m.type == kTypeNormal) {
                pic = ExtractChannels(img, {0, 1});
                RebuildNormalZ(pic);
                layout = PngLayout::rgb24;
            } else if (m.channels.size() == 1) {
                pic = ExtractChannels(img, m.channels);
                layout = PngLayout::gray8;
            } else {
                pic = ExtractChannels(img, m.channels);
                layout = PngLayout::rgb24;
            }
            if (keep_colour && m.type == kTypeColor && keep_colour->w == 0) *keep_colour = pic;
            MapOut mo;
            mo.rel = std::string(job.folder) + "\\" + name;
            mo.slot = slot ? slot->id : "ref";
            mo.component = m.type == kTypeNormal ? std::string("RG") : m.Component();
            mo.w = pic.w;
            mo.h = pic.h;
            if (!EncodePng(pic, layout, mo.png, &err)) return why = "PNG: " + err, false;
            maps.push_back(std::move(mo));
        }
    }
    if (maps.empty() && skips.empty()) return why = "no maps", false;
    return true;
}

std::string ResolveNumbered(const std::wstring& out_dir, const std::string& parent, const NamedFolder& f) {
    for (const std::wstring& d : ListDir(JoinPath(out_dir, parent), true))
        if (FolderNumber(Narrow(d)) == f.number) return parent + "\\" + Narrow(d);
    return parent + "\\" + f.name;
}

bool FreeSpaceOk(const std::wstring& dir, std::uint64_t need, std::string& error) {
    std::wstring probe = dir;
    while (!probe.empty() && !IsDir(probe)) {
        const std::size_t s = probe.find_last_of(L"\\/");
        if (s == std::wstring::npos) break;
        probe.resize(s);
    }
    ULARGE_INTEGER avail{};
    if (probe.empty() || !GetDiskFreeSpaceExW((probe + L"\\").c_str(), &avail, nullptr, nullptr)) return true;   // unknown: try
    if (avail.QuadPart < need) {
        error = "Not enough free disk space for the textures: " + std::to_string(avail.QuadPart >> 20) + " MB free, " +
                std::to_string(need >> 20) + " MB needed.";
        return false;
    }
    return true;
}

}  // namespace

bool RunExtract(const ExtractOptions& o, ExtractStats& stats, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    stats = ExtractStats{};
    Out out(o, stats);
    if (o.out_dir.empty()) return error = "No textures folder.", false;
    if (!FreeSpaceOk(o.out_dir, o.min_free_bytes, error)) return false;
    // A folder that already holds other things and was never extracted into is refused: the extraction writes README.txt
    // and our folders there and cleans up its own leftovers, never anything of someone else's.
    if (IsDir(o.out_dir) && !IsFile(out.Abs(kManifestName))) {
        for (const std::wstring& n : ListAll(o.out_dir))
            if (!IsOurTopName(n))
                return error = "The textures folder " + Narrow(o.out_dir) + " already has other files in it (" + Narrow(n) +
                               "): pick an empty or new folder for the textures.", false;
    }
    if (!MakeDirs(o.out_dir)) return error = "Cannot create the textures folder " + Narrow(o.out_dir) + ".", false;
    DeleteOurPartials(o.out_dir);   // a cancelled run's leftovers (only in our own folders)

    // The previous manifest (re-run rules).
    Manifest old;
    {
        std::vector<std::uint8_t> bytes;
        std::string err;
        if (ReadWhole(out.Abs(kManifestName), bytes) && !ParseManifest(std::string(bytes.begin(), bytes.end()), old, err)) old = Manifest{};
    }
    const std::string stamp = GameStamp(o.game_dir);
    // reused only when the same game, slot table AND extractor output wrote them (a launcher update that changes the
    // pictures or their names extracts again)
    const bool same_game = old.table == kTableVersion && old.extract == kExtractVersion && old.game == stamp && stamp != "unknown";

    Oodle oodle;
    Decompressor dec = o.decompressor;
    if (!dec) {
        if (!oodle.Load(o.game_dir, error)) return false;
        dec = oodle.Get();
    }
    ArchiveSet arc;
    {
        std::string err;
        if (!arc.Open(JoinPath(o.game_dir, "data"), err)) return error = "Cannot read the game's data folder: " + err, false;
    }
    const std::vector<ExtractJob>& jobs = o.jobs ? *o.jobs : ExtractJobs();
    {
        std::set<std::uint64_t> wanted;
        for (const ExtractJob& j : jobs) wanted.insert(CorePathHash(j.core_path));
        arc.Index(wanted);
    }

    Manifest nm;
    nm.table = kTableVersion;
    nm.game = stamp;
    nm.tool = o.tool_version;
    nm.extract = kExtractVersion;
    std::set<std::string> found_slots;   // check_slots: the slots of Fragile's sets seen (and matched)
    const int total = static_cast<int>(jobs.size()) + 3;
    int done = 0;
    Image sam_palette_colour;
    std::map<std::string, bool> loaded_cores;

    for (std::size_t ji = 0; ji < jobs.size(); ++ji) {
        const ExtractJob& job = jobs[ji];
        out.cancel_armed_ = o.cancel_after_sets >= 0 && static_cast<int>(ji) == o.cancel_after_sets;
        out.Emit("progress " + std::to_string(done) + " " + std::to_string(total) + " " + job.folder + "\\" + job.part);
        const std::string prefix = std::string(job.folder) + "\\" + job.part + " - ";
        // Up to date: same game and table, and every file of this set still there with its size.
        if (same_game) {
            std::vector<const ManifestRow*> rows;
            bool all = true;
            for (const ManifestRow& r : old.rows) {
                if (r.path.size() <= prefix.size() || !SamePath(r.path.substr(0, prefix.size()), prefix)) continue;
                const std::string rest = r.path.substr(prefix.size());
                if (rest.find('\\') != std::string::npos) continue;
                std::uint64_t sz = 0;
                if (!IsFile(out.Abs(r.path), &sz) || sz != r.bytes) all = false;
                rows.push_back(&r);
            }
            if (all && !rows.empty()) {
                for (const ManifestRow* r : rows) {
                    nm.rows.push_back(*r);
                    if (job.original && r->slot != "ref") found_slots.insert(r->slot);   // checked when they were written
                }
                ++stats.sets_reused;
                ++done;
                continue;
            }
        }
        // Stream files are indexed per set (their names come from the set's core).
        LoadedSet ls;
        std::string why;
        if (!arc.Has(CorePathHash(job.core_path))) {
            why = "not in the game's archives";
        } else if (LoadSet(arc, dec, job.core_path, ls, why)) {
            std::set<std::uint64_t> wanted;
            for (const auto& [g, t] : ls.textures)
                if (t.stream_size && !arc.Has(CorePathHash(t.stream_path + ".core.stream"))) wanted.insert(CorePathHash(t.stream_path + ".core.stream"));
            if (!wanted.empty()) arc.Index(wanted);
            why.clear();
        }
        std::vector<MapOut> maps;
        std::vector<std::string> skips;
        const bool keep = o.previews && std::strcmp(job.part, "Suit") == 0 && std::strstr(job.folder, "Palette colours") != nullptr;
        bool game_changed = false;
        if (why.empty() &&
            !BuildMaps(arc, dec, job, ls, maps, skips, keep ? &sam_palette_colour : nullptr, o.check_slots ? &found_slots : nullptr, game_changed, why))
            maps.clear();
        if (game_changed)
            return error = "The game's data changed (Fragile's " + std::string(job.part) + " texture " + why +
                           "): this Stranded Together version cannot use it. Update Stranded Together, then extract again.",
                   false;
        if (!why.empty()) {
            out.Emit("skip " + std::string(job.core_path) + " " + why);
            ++stats.sets_skipped;
            if (job.original) return error = "Fragile's " + std::string(job.part) + " textures could not be read (" + why + ").", false;
            ++done;
            continue;
        }
        for (const std::string& s : skips) out.Emit("skip " + std::string(job.folder) + "\\" + s);
        std::uint64_t set_bytes = 0;
        for (MapOut& m : maps) {
            if (!out.Reference(m.rel, m.png, error)) return false;
            ManifestRow r;
            r.path = m.rel;
            r.slot = m.slot;
            r.component = m.component;
            r.w = m.w;
            r.h = m.h;
            r.sha256 = ShaHex(m.png);
            r.kind = job.original ? "original" : "ref";
            r.bytes = m.png.size();
            set_bytes += r.bytes;
            nm.rows.push_back(std::move(r));
        }
        char line[256];
        std::snprintf(line, sizeof(line), "set %s: %zu maps, %.1f MB", job.core_path + std::string(job.core_path).find_last_of('/') + 1, maps.size(),
                      set_bytes / 1048576.0);
        out.Emit(line);
        ++stats.sets_done;
        ++done;
    }
    out.cancel_armed_ = false;
    // Every slot of Fragile's own sets found (tex_slots.hpp's promise: a game update that moves one makes this refuse,
    // and no manifest with this table value is written).
    if (o.check_slots) {
        std::string missing;
        for (const Slot& s : kSlots) {
            bool wanted = false;
            for (const ExtractJob& j : jobs) wanted = wanted || (j.original && std::strcmp(j.core_path, s.set_core_path) == 0);
            if (wanted && !found_slots.count(s.id)) missing += (missing.empty() ? "" : ", ") + std::string(s.id);
        }
        if (!missing.empty())
            return error = "The game's data changed (Fragile's textures " + missing +
                           " are not where this version expects them): this Stranded Together version cannot use it. Update Stranded Together, then "
                           "extract again.",
                   false;
    }

    // ---- Seeds (never overwritten) ----
    out.Emit("progress " + std::to_string(done) + " " + std::to_string(total) + " editable folders");
    auto seed_folder = [&](const std::string& rel_dir, const std::vector<std::string>& names) -> bool {
        for (const std::string& n : names) {
            const std::string orig_rel = std::string(kOriginalDir) + "\\" + n;
            const ManifestRow* orow = nm.Find(orig_rel);
            if (!orow) continue;
            std::vector<std::uint8_t> bytes;
            if (!ReadWhole(out.Abs(orig_rel), bytes)) return error = "cannot read " + orig_rel, false;
            const std::string rel = rel_dir + "\\" + n;
            const int r = out.Seed(rel, bytes, error);
            if (r < 0) return false;
            ManifestRow row = *orow;
            row.path = rel;
            row.kind = "seed";
            if (r == 0) {
                ++stats.kept;
                out.Emit("kept " + rel);
                const ManifestRow* prev = old.Find(rel);
                if (prev && prev->kind == "seed") {
                    row.sha256 = prev->sha256;   // the bytes first written, so "edited" stays right after a game update
                    row.bytes = prev->bytes;
                    if (prev->sha256 != orow->sha256 && FileShaHex(out.Abs(rel)) == prev->sha256) ++stats.outdated_seeds;
                }
            }
            nm.rows.push_back(row);
        }
        return true;
    };
    for (const NamedFolder& f : kColourFolders) {
        const std::string dir = ResolveNumbered(o.out_dir, kFragileSuitDir, f);
        if (!seed_folder(dir, SuitSeedNames())) return false;
        // Sam's colour as a swatch (reference, regenerated).
        Image sw;
        sw.Resize(256, 256);
        std::uint8_t rgb[3];
        SwatchColour(f.number, rgb);
        Fill(sw, 0, 0, 256, 256, rgb);
        std::vector<std::uint8_t> png;
        std::string err;
        if (EncodePng(sw, PngLayout::rgb24, png, &err)) {
            const std::string rel = dir + "\\Sam's colour.png";
            if (!out.Reference(rel, png, error)) return false;
            nm.rows.push_back(ManifestRow{rel, "ref", "RGB", 256, 256, ShaHex(png), "preview", png.size()});
        }
    }
    for (const NamedFolder& f : kBootFolders) {
        const std::string dir = ResolveNumbered(o.out_dir, kFragileBootsDir, f);
        if (!seed_folder(dir, BootSeedNames())) return false;
        for (const char* sub : {"Gold", "Silver", "Worn"}) MakeDirs(out.Abs(dir + "\\" + sub));
    }
    ++done;

    // ---- Sam palette previews and swatches ----
    out.Emit("progress " + std::to_string(done) + " " + std::to_string(total) + " Sam palette previews");
    {
        const std::string pal = "Sam porter suit\\Palette colours (00-14)";
        Image sheet;
        sheet.Resize(5 * 160, 3 * 160);
        for (int c = 0; c < 15; ++c) {
            std::uint8_t rgb[3];
            SwatchColour(c, rgb);
            const std::uint32_t x = static_cast<std::uint32_t>(c % 5) * 160u, y = static_cast<std::uint32_t>(c / 5) * 160u;
            Fill(sheet, x, y, 160, 160, rgb);
            const double lum = 0.3 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2];
            const std::uint8_t ink[3] = {static_cast<std::uint8_t>(lum > 128 ? 20 : 240), static_cast<std::uint8_t>(lum > 128 ? 20 : 240),
                                         static_cast<std::uint8_t>(lum > 128 ? 20 : 240)};
            DrawNumber(sheet, c, x + 12, y + 12, 6, ink);
        }
        std::vector<std::uint8_t> png;
        std::string err;
        if (EncodePng(sheet, PngLayout::rgb24, png, &err)) {
            const std::string rel = pal + "\\Colour swatches.png";
            if (!out.Reference(rel, png, error)) return false;
            nm.rows.push_back(ManifestRow{rel, "ref", "RGB", sheet.w, sheet.h, ShaHex(png), "preview", png.size()});
        }
        if (o.previews) {
            const std::string prefix = pal + "\\Previews\\";
            bool reuse = same_game;
            std::vector<const ManifestRow*> rows;
            for (const ManifestRow& r : old.rows)
                if (r.path.size() > prefix.size() && SamePath(r.path.substr(0, prefix.size()), prefix)) rows.push_back(&r);
            if (rows.size() != 15) reuse = false;
            for (const ManifestRow* r : rows) {
                std::uint64_t sz = 0;
                if (!IsFile(out.Abs(r->path), &sz) || sz != r->bytes) reuse = false;
            }
            if (reuse) {
                for (const ManifestRow* r : rows) nm.rows.push_back(*r);
            } else {
                if (sam_palette_colour.w == 0) ReadPngFile(out.Abs(pal + "\\Suit - Colour.png"), sam_palette_colour, nullptr);
                if (sam_palette_colour.w) {
                    // Roughly what the game draws: his light grey map multiplied (in linear light) by the palette
                    // colour, scaled so his average grey becomes exactly that colour.
                    const Image base = Resize(sam_palette_colour, std::min(1024u, sam_palette_colour.w), std::min(1024u, sam_palette_colour.h),
                                              MipKind::srgb_rgb);
                    double mean[3] = {0, 0, 0};
                    const std::size_t n = static_cast<std::size_t>(base.w) * base.h;
                    for (std::size_t i = 0; i < n; ++i)
                        for (int k = 0; k < 3; ++k) mean[k] += SrgbToLinear(base.rgba[i * 4 + static_cast<std::size_t>(k)]);
                    for (double& m : mean) m = std::max(1e-4, m / static_cast<double>(n));
                    for (int c = 0; c < 15; ++c) {
                        std::uint8_t rgb[3];
                        SwatchColour(c, rgb);
                        Image pv = base;
                        for (std::size_t i = 0; i < n; ++i)
                            for (int k = 0; k < 3; ++k) {
                                std::uint8_t& v = pv.rgba[i * 4 + static_cast<std::size_t>(k)];
                                v = LinearToSrgb8(static_cast<float>(SrgbToLinear(v) * SrgbToLinear(rgb[k]) / mean[k]));
                            }
                        std::vector<std::uint8_t> ppng;
                        if (!EncodePng(pv, PngLayout::rgb24, ppng, &err)) continue;
                        const std::string rel = prefix + kColourFolders[c].name + ".png";
                        if (!out.Reference(rel, ppng, error)) return false;
                        nm.rows.push_back(ManifestRow{rel, "ref", "RGB", pv.w, pv.h, ShaHex(ppng), "preview", ppng.size()});
                    }
                }
            }
        }
    }
    ++done;

    // ---- README and manifest ----
    out.Emit("progress " + std::to_string(done) + " " + std::to_string(total) + " README and index");
    {
        const std::string text = ReadmeText();
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        // a read-only reference like Original\ (regenerated by every extraction: notes belong in a file of your own)
        if (!out.Reference(kReadmeName, bytes, error)) return false;
        const std::string m = WriteManifest(nm);
        std::vector<std::uint8_t> mb(m.begin(), m.end());
        const std::wstring abs = out.Abs(kManifestName);
        if (!WriteWhole(abs + L".partial", mb)) return error = "cannot write the index (.manifest.txt)", false;
        SetFileAttributesW(abs.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW((abs + L".partial").c_str(), abs.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW((abs + L".partial").c_str());
            return error = "cannot replace the index (.manifest.txt)", false;
        }
        SetFileAttributesW(abs.c_str(), FILE_ATTRIBUTE_HIDDEN);
    }
    ++done;
    stats.ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    out.Emit("progress " + std::to_string(done) + " " + std::to_string(total) + " done");
    if (stats.outdated_seeds)
        out.Emit("note " + std::to_string(stats.outdated_seeds) + " unedited seeds are from an older game version: delete them to refresh");
    out.Emit("done " + std::to_string(stats.written) + " " + std::to_string(stats.kept) + " " + std::to_string(stats.bytes) + " " + std::to_string(stats.ms));
    return true;
}

// ---- Status / Check my edits -------------------------------------------------------------------------------------
void ScanEdits(const std::wstring& dir, bool decode, EditScan& s) {
    s = EditScan{};
    std::vector<std::uint8_t> bytes;
    const std::wstring mpath = JoinPath(dir, kManifestName);
    if (!ReadWhole(mpath, bytes)) {
        s.manifest_error = "not extracted yet";
        return;
    }
    if (!ParseManifest(std::string(bytes.begin(), bytes.end()), s.manifest, s.manifest_error)) return;
    s.extracted = true;
    s.table_ok = s.manifest.table == kTableVersion;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(mpath.c_str(), GetFileExInfoStandard, &a)) {
        FILETIME local;
        SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            s.extracted_utc = buf;
        }
    }
    auto scan_folder = [&](const std::string& rel_dir, bool boots) {
        EditedFolder ef;
        ef.folder = rel_dir;
        for (const std::wstring& wf : ListDir(JoinPath(dir, rel_dir), false)) {
            const std::string f = Narrow(wf);
            if (f.size() < 4 || _stricmp(f.c_str() + f.size() - 4, ".png") != 0) continue;
            if (SamePath(f, "Sam's colour.png")) continue;
            const std::string rel = rel_dir + "\\" + f;
            const std::string short_rel = rel.substr(rel.find('\\') + 1);
            if (!(boots ? IsUsableBootName(f) : IsUsableSuitName(f))) {
                s.problems.push_back(short_rel + " is not used (the names the mod reads are those in Original\\)");
                continue;
            }
            const ManifestRow* orig = s.manifest.Find(std::string(kOriginalDir) + "\\" + f);
            const ManifestRow* seed = s.manifest.Find(rel);
            if (!orig) continue;
            const std::wstring abs = JoinPath(dir, rel);
            const std::string sha = FileShaHex(abs);
            if (sha.empty()) {
                s.problems.push_back(short_rel + " cannot be read");
                continue;
            }
            const std::string& ref = (seed && seed->kind == "seed") ? seed->sha256 : orig->sha256;
            if (seed && seed->kind == "seed" && sha == seed->sha256 && seed->sha256 != orig->sha256) ++s.outdated_seeds;
            if (sha == ref) continue;
            std::string map = f.substr(f.find(" - ") + 3);
            map.resize(map.size() - 4);
            ef.maps.push_back(map);
            std::vector<std::uint8_t> head;
            std::uint32_t w = 0, h = 0;
            if (ReadWhole(abs, head) && PngSize(head.data(), head.size(), w, h)) {
                const bool too_big = w > MaxEditSide(orig->w) || h > MaxEditSide(orig->h);
                if (too_big)
                    s.problems.push_back(short_rel + " is " + std::to_string(w) + "x" + std::to_string(h) + ", too big: the mod skips it (save it at " +
                                         std::to_string(orig->w) + "x" + std::to_string(orig->h) + ")");
                else if (w != orig->w || h != orig->h)
                    s.problems.push_back(short_rel + " is " + std::to_string(w) + "x" + std::to_string(h) + ": it will be resized to " +
                                         std::to_string(orig->w) + "x" + std::to_string(orig->h));
                if (decode && !too_big) {
                    Image img;
                    std::string err;
                    if (!DecodePng(head.data(), head.size(), img, &err)) s.problems.push_back(short_rel + " cannot be read as a PNG (" + err + ")");
                }
            } else {
                s.problems.push_back(short_rel + " is not a PNG picture");
            }
        }
        if (!ef.maps.empty()) s.edited.push_back(std::move(ef));
    };
    for (const std::wstring& d : ListDir(JoinPath(dir, kFragileSuitDir), true)) {
        const int n = FolderNumber(Narrow(d));
        if (n < 0 || n > 16) continue;
        ++s.colour_folders;
        scan_folder(std::string(kFragileSuitDir) + "\\" + Narrow(d), false);
    }
    for (const std::wstring& d : ListDir(JoinPath(dir, kFragileBootsDir), true)) {
        if (FolderNumber(Narrow(d)) < 0) continue;
        ++s.boot_folders;
        const std::string rel = std::string(kFragileBootsDir) + "\\" + Narrow(d);
        scan_folder(rel, true);
        for (const char* sub : {"Gold", "Silver", "Worn"}) scan_folder(rel + "\\" + sub, true);
    }
}

std::string EditSummary(const EditScan& s) {
    if (s.edited.empty()) return {};
    std::string out = "Edited: ";
    for (std::size_t i = 0; i < s.edited.size(); ++i) {
        const EditedFolder& f = s.edited[i];
        if (i) out += ", ";
        out += f.folder.substr(f.folder.find('\\') + 1) + " (";
        for (std::size_t k = 0; k < f.maps.size(); ++k) out += (k ? ", " : "") + f.maps[k];
        out += ")";
    }
    return out;
}

std::string CheckReport(const EditScan& s) {
    if (!s.extracted) return "The textures are not extracted yet: press Extract textures.";
    std::string r;
    if (!s.table_ok) r += "These textures were extracted by another version: press Extract textures again.\n";
    r += std::to_string(s.colour_folders) + " suit colour folders, " + std::to_string(s.boot_folders) + " boot type folders";
    if (!s.extracted_utc.empty()) r += " (extracted " + s.extracted_utc + ")";
    r += ".\n";
    const std::string e = EditSummary(s);
    r += e.empty() ? std::string("No edited pictures yet: every folder shows Fragile's own look.\n") : e + ".\n";
    if (s.outdated_seeds)
        r += std::to_string(s.outdated_seeds) + " unedited pictures are from an older game version: delete them and extract again to refresh.\n";
    for (const std::string& p : s.problems) r += p + ".\n";
    return r;
}

}  // namespace st::tex
