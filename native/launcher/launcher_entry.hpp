#pragma once
// Stranded Together: the windowless command-line modes, and the single-instance
// claim the window shell needs before its model exists, and the new launcher's
// side of the restart after a self-update. Adapted from Diamond Dogs
// Together's launcher_entry (the local-test entries are gone).
#include <cstdint>
#include <functional>
#include <string>

#include "launcher_ui_logic.hpp"

namespace st {

// --watch <session>: the hidden helper that cleans up after the game exits.
int RunWatch(const std::wstring& session);

// Developer only: --write-session / --end-session (no window, no Steam launch).
int RunWriteSession(const ui::Options& options, std::string& report);
int RunEndSession(const std::wstring& game_dir, std::string& report);

// --extract-textures / --check-textures (Fragile's textures, core_textures.hpp): lines go to `print` as they come
// (the launcher window reads them from its child's stdout pipe).
int RunTextures(const ui::Options& options, const std::function<void(const std::string&)>& print);

// --selftest: the mock model through every phase plus the core's pure checks
// and a read-only report on this folder. 0 = passed. `report` gets one line per
// failure and the summaries.
int RunSelfTest(std::string& report);

// One launcher window per game folder: claims the folder's single-instance
// mutex `name` and returns it owned (a HANDLE), or nullptr when another window
// keeps the folder (bring_forward() then puts that window in front).
void* ClaimFolderInstance(const std::wstring& name, const std::function<bool()>& bring_forward);

// --after-update <pid>: tells the launcher that installed this version (pid) that
// this one runs, and waits until it has closed (ends it when it hangs), so the
// folder's single instance and StrandedTogether.exe.old are free (core_update.hpp).
void TakeOverAfterUpdate(std::uint32_t pid);

}  // namespace st
