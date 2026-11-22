// Stranded Together textures: the game's own Oodle decompressor (<game>\oo2core_7_win64.dll), loaded by full path only
// in the launcher's "Extract textures" child process. The DLL is never copied; it is unloaded when this object goes.
#pragma once
#include <string>

#include "decima_archive.hpp"

namespace st::tex {

class Oodle {
public:
    Oodle() = default;
    ~Oodle();
    Oodle(const Oodle&) = delete;
    Oodle& operator=(const Oodle&) = delete;
    // Refuses a game folder without oo2core_7_win64.dll directly inside it.
    bool Load(const std::wstring& game_dir, std::string& error);
    Decompressor Get() const;   // valid while this object lives

private:
    void* module_ = nullptr;
    void* fn_ = nullptr;
};

}  // namespace st::tex
