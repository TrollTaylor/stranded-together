// st_textool: development helper for Fragile's textures (docs/textures.md, docs/hidden-testing.md "Fragile's textures").
//   st_textool pattern --dir <textures folder> --name checker|stripes|metal --folder "<NN name>"
// paints a bold generated pattern (no game data) over the editable pictures of one colour folder ("16 Gold") of
// "Fragile porter suit", or of one boots folder of "Fragile boots" (or its Worn, Gold or Silver subfolder: "09 Chiral
// boots\Worn"), as a user's edit would: the hidden scenarios use it
// to give the mod an edited set it can recognise in a screenshot. Only inside the given folder; nothing else is touched.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "png_wic.hpp"
#include "tex_codec.hpp"
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

int Pattern(const std::wstring& dir, const std::string& name, const std::wstring& folder_in) {
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
    std::wstring target = Find(dir + L"\\Fragile porter suit", number);
    bool boots = false;
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
    bool ok = true;
    if (boots) {
        ok = Write(target + L"\\Boots - Colour.png", Fill(1024, name.c_str(), 0), tx::PngLayout::rgb24) && ok;
        ok = Write(target + L"\\Boots - Roughness.png", Fill(1024, name.c_str(), 1), tx::PngLayout::gray8) && ok;
    } else {
        ok = Write(target + L"\\Suit - Colour.png", Fill(2048, name.c_str(), 0), tx::PngLayout::rgb24) && ok;
        ok = Write(target + L"\\Suit - Roughness.png", Fill(2048, name.c_str(), 1), tx::PngLayout::gray8) && ok;
        if (name == "metal") ok = Write(target + L"\\Suit - Reflectance.png", Fill(1024, name.c_str(), 2), tx::PngLayout::rgb24) && ok;
    }
    std::printf("%s pattern %s in %ls\n", ok ? "done:" : "FAILED:", name.c_str(), target.c_str());
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || std::wstring(argv[1]) != L"pattern") {
        std::printf("st_textool pattern --dir <textures folder> --name checker|stripes|metal --folder \"<NN name>\"\n");
        return 2;
    }
    std::wstring dir, folder;
    std::string name;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i];
        if (k == L"--dir") dir = argv[i + 1];
        else if (k == L"--folder") folder = argv[i + 1];
        else if (k == L"--name") {
            for (const wchar_t* p = argv[i + 1]; *p; ++p) name.push_back(static_cast<char>(*p));
        }
    }
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty() || folder.empty() || (name != "checker" && name != "stripes" && name != "metal")) {
        std::printf("missing or bad arguments\n");
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
    const int rc = Pattern(dir, name, folder);
    CoUninitialize();
    return rc;
}
