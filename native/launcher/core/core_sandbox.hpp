// Stranded Together launcher core: test saves (sandboxes) and the local two-instance test
// (docs/sandbox-and-local-test.md).
//
// A sandbox is <game>\stranded_together\sandbox\<name>\:
//   saves\                         what the game sees as %LOCALAPPDATA%\KojimaProductions while it
//                                  runs with session.ini sandbox_saves=<name> (the mod redirects
//                                  every file access there, native/mod/sandbox.hpp)
//     .st-redirect-probe           the mod's self-test file (only the sandbox has it)
//     DeathStrandingDC\<account>\  profile\ copied from YOUR saves (read only) + the save slots of
//                                  the chosen test save (autosave*/manualsave*/quicksave*)
//     DeathStranding\<account>\transfer\
//   settings.cfg                   the game folder's settings.cfg as this sandbox's games see it
//                                  (appdir_settings=1: local two-instance test)
//   sandbox.json                   what it was seeded from (launcher only)
// Your real save tree is only ever READ (copied from); nothing here writes below it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core_env.hpp"
#include "core_paths.hpp"

namespace st::core {

inline constexpr char kTestSaveSandbox[] = "test";          // Play with test save
inline constexpr char kLocalHostSandbox[] = "local-host";   // local two-instance test, first game
inline constexpr char kLocalGuestSandbox[] = "local-guest"; // local two-instance test, second game
inline constexpr wchar_t kProbeName[] = L".st-redirect-probe";

struct SandboxPaths {
    std::wstring root;       // M\sandbox\<name>
    std::wstring saves;      // root\saves
    std::wstring settings;   // root\settings.cfg
    std::wstring marker;     // root\sandbox.json
};
SandboxPaths GetSandboxPaths(const InstancePaths& paths, const std::string& name);
bool SandboxSeeded(const SandboxPaths& sandbox);   // the probe file is there

// A folder the user picked as the test save. Accepted layouts (the downloaded 100% save,
// a copy of a save folder): <folder>\DeathStrandingDC\<slots>, <folder>\DeathStrandingDC\<account>\<slots>,
// <folder> = ...\DeathStrandingDC (with <slots> or <account>\<slots>), or <folder> = the account
// folder itself (<folder>\<slots>). A slot is autosaveN / manualsaveN / quicksaveN holding checkpoint.dat.
struct SeedCheck {
    bool ok = false;
    std::wstring slots_dir;   // the folder whose sub-folders are the slots
    int slots = 0;
    std::string note;         // a sentence for the player (why not / "12 save slots")
};
SeedCheck CheckSeedFolder(const std::wstring& folder);

// The user's real save tree: %LOCALAPPDATA%\KojimaProductions (from the environment; "" unknown).
std::wstring RealSaveRoot();

// Fills a sandbox: profile\ (and the other non-slot files: game_settings.cfg lives in profile\)
// from <real_root>\DeathStrandingDC\<account>\, the slots from `seed` (when given; otherwise the
// user's own slots too: a full copy of their saves), the probe. The sandbox is emptied first.
// Refuses when the sandbox is not below M\sandbox or overlaps the real root. Reads only from
// real_root. account = the Steam account id (SteamID64 - 76561197960265728), 0 = every account
// folder found.
bool SeedSandbox(const InstancePaths& paths, const SandboxPaths& sandbox, const SeedCheck* seed, const std::wstring& real_root,
                 std::uint32_t account, const std::string& seed_label, std::string& error);
// Deletes a sandbox (only below M\sandbox).
bool ResetSandbox(const InstancePaths& paths, const SandboxPaths& sandbox, std::string& error);

// "relative\path\tsize\tSHA-256" for every file below root, sorted ("" for a missing root).
std::string SaveTreeManifest(const std::wstring& root);
// Lines that differ between two manifests ("changed|added|removed <path>").
std::vector<std::string> ManifestChanges(const std::string& before, const std::string& after);

// The local two-instance test: low-quality windowed graphics for the sandbox's settings.cfg
// (sandbox.hpp) merged into `current` (the file as the game last wrote it; "" when missing):
// windowed, 1280x720, low quality. Written before every local test.
std::string LocalTestSettingsCfg(const std::string& current);

}  // namespace st::core
