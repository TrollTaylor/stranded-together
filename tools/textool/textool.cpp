// st_textool: development helper for Fragile's textures (docs/textures.md, docs/hidden-testing.md "Fragile's textures").
//   st_textool pattern --dir <textures folder> --name checker|stripes|metal|ao --folder "<NN name>"
// paints a bold generated pattern (no game data) over the editable pictures of one colour folder ("16 Gold") of
// "Fragile porter suit", or of one boots folder of "Fragile boots" (or its Worn, Gold or Silver subfolder: "09 Chiral
// boots\Worn"), as a user's edit would: the hidden scenarios use it
// to give the mod an edited set it can recognise in a screenshot. Only inside the given folder; nothing else is touched.
// "ao" (round 8) writes only a suit folder's "Suit - Ambient occlusion.png" (a grey checker: a folder painted in a
// picture the extractor does not seed, so the folder becomes the user's).
//   st_textool copy --dir <textures folder> --to "<NN name>\<Suit - X.png>" --from "<picture file>"
// copies one picture file (only read; any format, e.g. an image tool's JPEG) under one of the names the mod reads.
// Round 8 (the ready-made colours, docs/textures.md):
//   st_textool v024 --dir <textures folder>
// turns an extracted folder back into what v0.24's extractor left: Original's three Suit pictures copied over every
// colour folder 00..16, their seed rows set to Original's hashes, the manifest's extract field 1 (the upgrade tests).
//   st_textool reset --dir <textures folder> --folder "<NN name>"
// deletes that colour folder's three Suit pictures (the mod then wears the ready-made colour again).
//   st_textool gen1 --dir <textures folder>
// round 9: 15 Silver / 16 Gold as round 8's extractor left them (generator 1's satin metal = metal variant 3 today, their
// seed rows, manifest extract 2): the refresh tests (the next extraction replaces them; the mod wears the current one).
//   st_textool defaults --dir <textures folder> --out <folder>
// development: writes every colour's ready-made pictures (suit_defaults.hpp) from Original\ into <out>\NN\ (PNG),
// e.g. to compare them with a prototype. <out> must not be inside the textures folder.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "png_wic.hpp"
#include "suit_defaults.hpp"
#include "tex_codec.hpp"
#include "tex_hash.hpp"
#include "tex_manifest.hpp"

namespace tx = st::tex;

namespace {

std::wstring Find(const std::wstring& base, int number) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const std::wstring n = fd.cFileName;
        if (n.size() >= 3 && n[0] >= L'0' && n[0] <= L'9' && n[1] >= L'0' && n[1] <= L'9' && n[2] == L' ' && (n[0] - L'0') * 10 + (n[1] - L'0') == number) {
            found = base + L"\\" + n;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

bool Write(const std::wstring& path, const tx::Image& img, tx::PngLayout layout) {
    std::vector<std::uint8_t> png;
    std::string err;
    if (!tx::EncodePng(img, layout, png, &err)) {
        std::printf("encode failed: %s\n", err.c_str());
        return false;
    }
    const std::wstring tmp = path + L".partial";
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, png.data(), static_cast<DWORD>(png.size()), &w, nullptr) && w == png.size();
    CloseHandle(h);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    std::printf("wrote %ls (%zu B)\n", path.c_str(), png.size());
    return true;
}

tx::Image Fill(std::uint32_t size, const char* name, int which) {
    // which: 0 colour, 1 roughness (grey), 2 reflectance
    tx::Image img;
    img.Resize(size, size);
    const std::string n = name;
    const std::uint32_t cell = size / 16;
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            std::uint8_t* p = img.At(x, y);
            std::uint8_t r = 0, g = 0, b = 0;
            if (n == "checker") {
                const bool c = ((x / cell) ^ (y / cell)) & 1;
                const bool band = ((x + y) / (cell / 2)) % 8 == 0;
                if (which == 0) r = band ? 255 : c ? 230 : 20, g = band ? 230 : c ? 20 : 220, b = band ? 0 : c ? 200 : 40;
                else r = g = b = 140;
            } else if (n == "ao") {
                r = g = b = (((x / cell) ^ (y / cell)) & 1) ? 235 : 70;
            } else if (n == "stripes") {
                const bool s = ((x + y) / (cell / 2)) & 1;
                if (which == 0) r = s ? 250 : 15, g = s ? 215 : 15, b = s ? 0 : 15;
                else r = g = b = 230;
            } else {  // metal: dark albedo, bright gold reflectance, low roughness
                if (which == 0) r = 40, g = 32, b = 20;
                else if (which == 1) r = g = b = 40;
                else r = 255, g = 200, b = 80;
            }
            p[0] = r, p[1] = g, p[2] = b, p[3] = 255;
        }
    return img;
}

// The folder a scenario names ("16 Gold", "09 Chiral boots", "09 Chiral boots\Worn") inside `dir`: 0 and `target`, or an
// exit code after a line.
int Resolve(const std::wstring& dir, const std::wstring& folder_in, std::wstring& target, bool& boots) {
    // a boots folder's subfolder: "09 Chiral boots\Worn" (Worn, Gold or Silver only)
    std::wstring folder = folder_in, sub;
    const std::size_t bs = folder_in.find(L'\\');
    if (bs != std::wstring::npos) {
        folder = folder_in.substr(0, bs);
        sub = folder_in.substr(bs + 1);
        if (sub != L"Worn" && sub != L"Gold" && sub != L"Silver") return std::printf("a subfolder is Worn, Gold or Silver\n"), 2;
    }
    if (folder.size() < 3 || folder[2] != L' ') return std::printf("the folder is like \"16 Gold\"\n"), 2;
    const int number = (folder[0] - L'0') * 10 + (folder[1] - L'0');
    target = Find(dir + L"\\Fragile porter suit", number);
    boots = false;
    // a boots folder is named in the scenario with its boots name ("09 Chiral boots"); a colour folder with its colour
    if (target.empty() || folder.find(L"boots") != std::wstring::npos || folder.find(L"Boots") != std::wstring::npos) {
        const std::wstring b = Find(dir + L"\\Fragile boots", number);
        if (!b.empty() && (target.empty() || folder.find(L"oots") != std::wstring::npos)) {
            target = b;
            boots = true;
        }
    }
    if (target.empty()) return std::printf("no folder %02d in %ls\n", number, dir.c_str()), 3;
    if (!sub.empty()) {
        if (!boots) return std::printf("only a boots folder has a Worn, Gold or Silver subfolder\n"), 2;
        target += L"\\" + sub;
        CreateDirectoryW(target.c_str(), nullptr);
    }
    return 0;
}

int Pattern(const std::wstring& dir, const std::string& name, const std::wstring& folder_in) {
    std::wstring target;
    bool boots = false;
    if (const int rc = Resolve(dir, folder_in, target, boots)) return rc;
    bool ok = true;
    if (boots) {
        ok = Write(target + L"\\Boots - Colour.png", Fill(1024, name.c_str(), 0), tx::PngLayout::rgb24) && ok;
        ok = Write(target + L"\\Boots - Roughness.png", Fill(1024, name.c_str(), 1), tx::PngLayout::gray8) && ok;
    } else if (name == "ao") {
        ok = Write(target + L"\\Suit - Ambient occlusion.png", Fill(1024, name.c_str(), 1), tx::PngLayout::gray8) && ok;
    } else {
        ok = Write(target + L"\\Suit - Colour.png", Fill(2048, name.c_str(), 0), tx::PngLayout::rgb24) && ok;
        ok = Write(target + L"\\Suit - Roughness.png", Fill(2048, name.c_str(), 1), tx::PngLayout::gray8) && ok;
        if (name == "metal") ok = Write(target + L"\\Suit - Reflectance.png", Fill(1024, name.c_str(), 2), tx::PngLayout::rgb24) && ok;
    }
    std::printf("%s pattern %s in %ls\n", ok ? "done:" : "FAILED:", name.c_str(), target.c_str());
    return ok ? 0 : 1;
}

// "copy": one picture file (only read) copied byte for byte under one of the names the mod reads, e.g. a JPEG an image
// tool wrote, as "16 Gold\Suit - Colour.png" (round 7: the mod reads a picture by its content). Atomic (.partial + move).
int Copy(const std::wstring& dir, const std::wstring& to, const std::wstring& from) {
    const std::size_t bs = to.find_last_of(L'\\');
    if (bs == std::wstring::npos) return std::printf("--to is <NN folder>\\<picture>.png\n"), 2;
    const std::wstring name = to.substr(bs + 1);
    if ((name.rfind(L"Suit - ", 0) != 0 && name.rfind(L"Boots - ", 0) != 0) || name.size() < 12 ||
        _wcsicmp(name.c_str() + name.size() - 4, L".png") != 0 || name.find_first_of(L"/:*?\"<>|") != std::wstring::npos ||
        name.find(L"..") != std::wstring::npos)
        return std::printf("the picture is named like \"Suit - Colour.png\"\n"), 2;
    std::wstring target;
    bool boots = false;
    if (const int rc = Resolve(dir, to.substr(0, bs), target, boots)) return rc;
    if (boots != (name.rfind(L"Boots - ", 0) == 0)) return std::printf("a boots folder holds Boots - pictures, a colour folder Suit - pictures\n"), 2;
    HANDLE h = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::printf("cannot open %ls (error %lu)\n", from.c_str(), GetLastError()), 3;
    LARGE_INTEGER sz{};
    std::vector<std::uint8_t> bytes;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (256ll << 20);
    if (ok) {
        bytes.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(h);
    if (!ok) return std::printf("cannot read %ls (empty or over 256 MB?)\n", from.c_str()), 3;
    const tx::PictureFormat fmt = tx::SniffPicture(bytes.data(), bytes.size());
    const std::wstring dest = target + L"\\" + name, tmp = dest + L".partial";
    h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::printf("cannot write %ls\n", tmp.c_str()), 1;
    DWORD put = 0;
    ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr) && put == bytes.size();
    CloseHandle(h);
    if (ok) {
        SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
        ok = MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    }
    if (!ok) DeleteFileW(tmp.c_str());
    std::printf("%s copied %ls (%zu B, %s) to %ls\n", ok ? "done:" : "FAILED:", from.c_str(), bytes.size(), tx::PictureFormatName(fmt), dest.c_str());
    return ok ? 0 : 1;
}

// ---- round 8 -------------------------------------------------------------------------------------------------
bool ReadBytes(const std::wstring& path, std::vector<std::uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (256ll << 20);
    if (ok) {
        out.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}
bool WriteBytesAtomic(const std::wstring& path, const std::vector<std::uint8_t>& b, DWORD attrs = FILE_ATTRIBUTE_NORMAL) {
    const std::wstring tmp = path + L".partial";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = b.empty() || (WriteFile(h, b.data(), static_cast<DWORD>(b.size()), &put, nullptr) && put == b.size());
    CloseHandle(h);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    else SetFileAttributesW(path.c_str(), attrs);
    return ok;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
const wchar_t* const kSuitPics[3] = {L"Suit - Colour.png", L"Suit - Reflectance.png", L"Suit - Roughness.png"};

// v0.24's folder: copies of her Original suit pictures in 00..16, seed rows = Original, extract 1.
int V024(const std::wstring& dir) {
    const std::wstring suit = dir + L"\\Fragile porter suit", orig = suit + L"\\Original";
    std::vector<std::uint8_t> mb;
    tx::Manifest m;
    std::string err;
    if (!ReadBytes(dir + L"\\.manifest.txt", mb) || !tx::ParseManifest(std::string(mb.begin(), mb.end()), m, err))
        return std::printf("no readable .manifest.txt in %ls (%s)\n", dir.c_str(), err.c_str()), 3;
    int copied = 0;
    for (int c = 0; c <= 16; ++c) {
        const std::wstring folder = Find(suit, c);
        if (folder.empty()) continue;
        const std::string rel_dir = Narrow(folder.substr(dir.size() + 1));
        for (const wchar_t* n : kSuitPics) {
            std::vector<std::uint8_t> b;
            if (!ReadBytes(orig + L"\\" + n, b)) return std::printf("cannot read Original\\%ls\n", n), 3;
            if (!WriteBytesAtomic(folder + L"\\" + n, b)) return std::printf("cannot write %ls\\%ls\n", folder.c_str(), n), 1;
            ++copied;
            const tx::ManifestRow* o = m.Find("Fragile porter suit\\Original\\" + Narrow(n));
            for (tx::ManifestRow& r : m.rows)
                if (o && r.kind == "seed" && tx::SamePath(r.path, rel_dir + "\\" + Narrow(n))) r.sha256 = o->sha256, r.bytes = o->bytes;
        }
    }
    m.extract = 1;
    const std::string t = tx::WriteManifest(m);
    if (!WriteBytesAtomic(dir + L"\\.manifest.txt", std::vector<std::uint8_t>(t.begin(), t.end()), FILE_ATTRIBUTE_HIDDEN))
        return std::printf("cannot write the manifest\n"), 1;
    std::printf("done: v0.24 folder: %d pictures copied from Original, manifest extract 1\n", copied);
    return 0;
}

// Round 9: the folder as round 8's extractor (extract 2, suit_defaults generator 1) left it: 15 Silver and 16 Gold hold
// generator 1's untouched gold and silver (the satin prototype, metal variant 3 today), their seed rows say so, extract 2.
// 01..14 are left as they are (generator 2 writes the same bytes there). Not inside any other folder.
int Gen1(const std::wstring& dir) {
    const std::wstring suit = dir + L"\\Fragile porter suit", orig = suit + L"\\Original\\";
    std::vector<std::uint8_t> mb;
    tx::Manifest m;
    std::string err;
    if (!ReadBytes(dir + L"\\.manifest.txt", mb) || !tx::ParseManifest(std::string(mb.begin(), mb.end()), m, err))
        return std::printf("no readable .manifest.txt in %ls (%s)\n", dir.c_str(), err.c_str()), 3;
    tx::Image c, f, r;
    if (!tx::ReadPngFile(orig + L"Suit - Colour.png", c, &err) || !tx::ReadPngFile(orig + L"Suit - Reflectance.png", f, &err) ||
        !tx::ReadPngFile(orig + L"Suit - Roughness.png", r, &err))
        return std::printf("her Original pictures do not read: %s\n", err.c_str()), 3;
    tx::defaults::Analysis a;
    if (!tx::defaults::Analyse(c, a, &err)) return std::printf("analysis: %s\n", err.c_str()), 3;
    int written = 0;
    for (int colour : {15, 16}) {
        const std::wstring folder = Find(suit, colour);
        if (folder.empty()) return std::printf("no colour folder %02d\n", colour), 3;
        tx::defaults::Output o;
        if (!tx::defaults::Make(colour, 3, a, c, f, r, o, &err)) return std::printf("colour %d: %s\n", colour, err.c_str()), 1;
        const tx::Image* imgs[3] = {&o.colour, &o.reflectance, &o.roughness};
        for (int k = 0; k < 3; ++k) {
            const std::wstring path = folder + L"\\" + kSuitPics[k];
            if (!Write(path, *imgs[k], k == 2 ? tx::PngLayout::gray8 : tx::PngLayout::rgb24)) return std::printf("cannot write %ls\n", path.c_str()), 1;
            tx::Sha256 h{};
            std::uint64_t size = 0;
            if (!tx::HashFileSha256(path, h, &size)) return std::printf("cannot hash %ls\n", path.c_str()), 1;
            ++written;
            const std::string rel = Narrow(path.substr(dir.size() + 1));
            for (tx::ManifestRow& row : m.rows)
                if (row.kind == "seed" && tx::SamePath(row.path, rel)) row.sha256 = tx::Hex(h), row.bytes = size;
        }
    }
    m.extract = 2;
    const std::string t = tx::WriteManifest(m);
    if (!WriteBytesAtomic(dir + L"\\.manifest.txt", std::vector<std::uint8_t>(t.begin(), t.end()), FILE_ATTRIBUTE_HIDDEN))
        return std::printf("cannot write the manifest\n"), 1;
    std::printf("done: round-8 folder: %d pictures of 15 Silver / 16 Gold are generator 1's (metal variant 3), manifest extract 2\n", written);
    return 0;
}

int Reset(const std::wstring& dir, const std::wstring& folder_in) {
    if (folder_in.size() < 3 || folder_in[2] != L' ' || folder_in.find(L'\\') != std::wstring::npos) return std::printf("the folder is like \"09 Mustard\"\n"), 2;
    const int number = (folder_in[0] - L'0') * 10 + (folder_in[1] - L'0');
    const std::wstring folder = Find(dir + L"\\Fragile porter suit", number);
    if (folder.empty()) return std::printf("no colour folder %02d\n", number), 3;
    int n = 0;
    for (const wchar_t* p : kSuitPics) {
        const std::wstring f = folder + L"\\" + p;
        SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(f.c_str())) ++n;
    }
    std::printf("done: %d Suit picture(s) deleted in %ls\n", n, folder.c_str());
    return 0;
}

int Defaults(const std::wstring& dir, const std::wstring& out) {
    const std::wstring orig = dir + L"\\Fragile porter suit\\Original\\";
    tx::Image c, f, r;
    std::string err;
    if (!tx::ReadPngFile(orig + L"Suit - Colour.png", c, &err) || !tx::ReadPngFile(orig + L"Suit - Reflectance.png", f, &err) ||
        !tx::ReadPngFile(orig + L"Suit - Roughness.png", r, &err))
        return std::printf("her Original pictures do not read: %s\n", err.c_str()), 3;
    const ULONGLONG t0 = GetTickCount64();
    tx::defaults::Analysis a;
    if (!tx::defaults::Analyse(c, a, &err)) return std::printf("analysis: %s\n", err.c_str()), 3;
    std::printf("analysis: %llu fabric pixels of %u x %u (%.1f%%), mean RGB %.2f %.2f %.2f, %llu ms\n", static_cast<unsigned long long>(a.n), a.w, a.h,
                100.0 * static_cast<double>(a.n) / (static_cast<double>(a.w) * a.h), static_cast<double>(a.sum[0]) / static_cast<double>(a.n),
                static_cast<double>(a.sum[1]) / static_cast<double>(a.n), static_cast<double>(a.sum[2]) / static_cast<double>(a.n),
                static_cast<unsigned long long>(GetTickCount64() - t0));
    CreateDirectoryW(out.c_str(), nullptr);
    bool ok = true;
    for (int colour = 1; colour <= 16; ++colour) {
        const ULONGLONG t1 = GetTickCount64();
        tx::defaults::Output o;
        if (!tx::defaults::Make(colour, 0, a, c, f, r, o, &err)) return std::printf("colour %d: %s\n", colour, err.c_str()), 1;
        wchar_t sub[16];
        swprintf_s(sub, L"\\%02d", colour);
        const std::wstring d = out + sub;
        CreateDirectoryW(d.c_str(), nullptr);
        if (o.maps & tx::defaults::kColour) ok = Write(d + L"\\Suit - Colour.png", o.colour, tx::PngLayout::rgb24) && ok;
        if (o.maps & tx::defaults::kReflectance) ok = Write(d + L"\\Suit - Reflectance.png", o.reflectance, tx::PngLayout::rgb24) && ok;
        if (o.maps & tx::defaults::kRoughness) ok = Write(d + L"\\Suit - Roughness.png", o.roughness, tx::PngLayout::gray8) && ok;
        std::printf("colour %02d: %llu ms\n", colour, static_cast<unsigned long long>(GetTickCount64() - t1));
    }
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring verb = argc >= 2 ? argv[1] : L"";
    if (verb != L"pattern" && verb != L"copy" && verb != L"v024" && verb != L"reset" && verb != L"defaults" && verb != L"gen1") {
        std::printf("st_textool pattern --dir <textures folder> --name checker|stripes|metal|ao --folder \"<NN name>\"\n"
                    "st_textool copy --dir <textures folder> --to \"<NN name>\\<Suit - X.png>\" --from \"<picture file>\"\n"
                    "st_textool v024 --dir <textures folder>\n"
                    "st_textool reset --dir <textures folder> --folder \"<NN name>\"\n"
                    "st_textool gen1 --dir <textures folder>\n"
                    "st_textool defaults --dir <textures folder> --out <folder>\n");
        return 2;
    }
    std::wstring dir, folder, to, from, out;
    std::string name;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--dir") dir = argv[i + 1];
        else if (k == L"--folder") folder = argv[i + 1];
        else if (k == L"--to") to = argv[i + 1];
        else if (k == L"--from") from = argv[i + 1];
        else if (k == L"--out") out = argv[i + 1];
        else if (k == L"--name") {
            for (const wchar_t* p = argv[i + 1]; *p; ++p) name.push_back(static_cast<char>(*p));
        }
    }
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (verb == L"v024") return dir.empty() ? (std::printf("missing --dir\n"), 2) : V024(dir);
    if (verb == L"gen1") {
        if (dir.empty()) return std::printf("missing --dir\n"), 2;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
        const int rc = Gen1(dir);
        CoUninitialize();
        return rc;
    }
    if (verb == L"reset") return dir.empty() || folder.empty() ? (std::printf("missing --dir or --folder\n"), 2) : Reset(dir, folder);
    if (verb == L"defaults") {
        while (!out.empty() && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
        if (dir.empty() || out.empty()) return std::printf("missing --dir or --out\n"), 2;
        wchar_t a[MAX_PATH], b[MAX_PATH];
        GetFullPathNameW(dir.c_str(), MAX_PATH, a, nullptr);
        GetFullPathNameW(out.c_str(), MAX_PATH, b, nullptr);
        if (_wcsnicmp(a, b, wcslen(a)) == 0) return std::printf("--out must not be inside the textures folder\n"), 2;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
        const int rc = Defaults(dir, out);
        CoUninitialize();
        return rc;
    }
    if (verb == L"copy") {
        if (dir.empty() || to.empty() || from.empty()) {
            std::printf("missing or bad arguments\n");
            return 2;
        }
        return Copy(dir, to, from);
    }
    if (dir.empty() || folder.empty() || (name != "checker" && name != "stripes" && name != "metal" && name != "ao")) {
        std::printf("missing or bad arguments\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
    const int rc = Pattern(dir, name, folder);
    CoUninitialize();
    return rc;
}
