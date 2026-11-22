// Stranded Together textures: the game's Oodle DLL (oodle_dyn.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "oodle_dyn.hpp"

namespace st::tex {
namespace {
// OodleLZ_Decompress as tools/re/ds_archive.py calls it (14 arguments).
using DecompressFn = std::intptr_t(__stdcall*)(const void* src, std::intptr_t src_size, void* dst, std::intptr_t dst_size, int fuzz,
                                                int crc, int verbose, void* dst_base, std::intptr_t dst_base_size, void* cb,
                                                void* cb_ctx, void* scratch, std::intptr_t scratch_size, int thread_phase);
}  // namespace

Oodle::~Oodle() {
    if (module_) FreeLibrary(static_cast<HMODULE>(module_));
}

bool Oodle::Load(const std::wstring& game_dir, std::string& error) {
    if (game_dir.empty()) return error = "no game folder", false;
    std::wstring path = game_dir;
    if (path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += L"oo2core_7_win64.dll";
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        return error = "the game folder has no oo2core_7_win64.dll (is it the DEATH STRANDING DIRECTOR'S CUT folder?)", false;
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return error = "could not load the game's oo2core_7_win64.dll (error " + std::to_string(GetLastError()) + ")", false;
    FARPROC fn = GetProcAddress(m, "OodleLZ_Decompress");
    if (!fn) {
        FreeLibrary(m);
        return error = "oo2core_7_win64.dll has no OodleLZ_Decompress", false;
    }
    module_ = m;
    fn_ = reinterpret_cast<void*>(fn);
    return true;
}

Decompressor Oodle::Get() const {
    auto fn = reinterpret_cast<DecompressFn>(fn_);
    return [fn](const std::uint8_t* src, std::size_t src_size, std::uint8_t* dst, std::size_t dst_size) {
        if (!fn) return false;
        const std::intptr_t got = fn(src, static_cast<std::intptr_t>(src_size), dst, static_cast<std::intptr_t>(dst_size), 1, 0, 0, nullptr,
                                     0, nullptr, nullptr, nullptr, 0, 3);
        return got == static_cast<std::intptr_t>(dst_size);
    };
}

}  // namespace st::tex
