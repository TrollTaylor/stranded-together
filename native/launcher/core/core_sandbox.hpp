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
//
// The host-held save session (M1, docs/coop-session.md) adds two folders:
//   sandbox\coop-guest\            the guest's private session folder (SeedSessionGuest): its own profile\ and
//                                  settings files (first use only; kept between sessions), and the copies of the
//                                  host's save the mod writes there (manualsave5/6, incoming\), wiped at every
//                                  session start
//   backups\<yyyyMMdd-HHmmss>\     a read-only copy of the host's own account folder before each co-op session
//                                  (BackupHostSaves; the newest kHostBackupsKept stay)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core_env.hpp"
#include "core_paths.hpp"
#include "core_text.hpp"

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

// autosaveN / manualsaveN / quicksaveN (any case): a save slot folder of the game.
bool IsSaveSlotName(const std::wstring& name);

// ---- The host-held save session (M1) ------------------------------------------------------
// The guest's private session folder (M1.2): <M>\sandbox\coop-guest.
//  - every slot folder (autosave*, manualsave*, quicksave*) of every account folder and incoming\ are deleted (the
//    session's copies of the host's save are a disposable cache: D1-9, wiped at the next session start); another
//    account's folder is deleted whole (its profile.dat would stall the game's boot);
//  - first use only (what is missing): the guest's OWN profile\ and the settings files next to it
//    (game_settings.cfg, bindings.cfg...) from <real_root>\DeathStrandingDC\<account>\, read only;
//    steam_autocloud.vdf and slots are never copied. What the game changes there stays in coop-guest (D1-6);
//  - the redirect probe (written last: a folder that could not be prepared is never taken for seeded).
// Never writes below real_root. account = the guest's Steam account id (0 = refused). Refused, before anything is
// deleted, when the co-op folder, a folder above it or ANY entry below its saves\ is a link (junction, symbolic link):
// the wipe never reaches through one.
struct SessionGuestReport {
    int wiped_slots = 0;        // slot folders deleted
    bool wiped_incoming = false;
    int removed_accounts = 0;   // other account folders deleted
    int copied = 0;             // profile\ and settings files copied (first use)
};
// `name`: the folder below stranded_together\sandbox (IsHostSaveGuestSandbox: coop-guest for Join, local-guest for the
// local two-player test's guest game).
bool SeedSessionGuest(const InstancePaths& paths, const std::wstring& real_root, std::uint32_t account, SessionGuestReport& report,
                      std::string& error, const char* name = kCoopGuestSandbox);
// The sandbox was last prepared by SeedSessionGuest (its sandbox.json says "session-guest": no save slot of its own).
bool SandboxIsSessionGuest(const SandboxPaths& sandbox);

// The host's backup before a co-op session (D1-2): <M>\backups\<stamp>\DeathStrandingDC\<account>\ is a copy of the
// host's account folder (every slot, profile\, the settings files; not steam_autocloud.vdf), read only, written as
// <stamp>.partial and renamed when complete, with backup.json ("seq": one more than the highest of the backups there).
// Then only the newest `keep` complete backups stay, newest by seq (not by the local-time stamp: a clock set ahead once,
// or a DST fall-back, gives older backups later names); the one just made is never pruned; only folders with our
// backup.json are ever deleted. A host without an account folder yet: nothing copied, true.
inline constexpr int kHostBackupsKept = 5;
struct HostBackupReport {
    std::wstring dir;     // the new backup ("" when there was nothing to copy)
    int slots = 0;
    int files = 0;        // files copied
    int pruned = 0;       // older backups deleted
    int newer_named = 0;  // backups kept whose time stamp is later than this one's (the clock went back): logged
};
std::wstring HostBackupsDir(const InstancePaths& paths);   // <M>\backups
// stamp: FormatStamp of the local time (yyyyMMdd-HHmmss; digits and '-' only).
bool BackupHostSaves(const InstancePaths& paths, const std::wstring& real_root, std::uint32_t account, const std::string& stamp, int keep,
                     HostBackupReport& report, std::string& error);

// SaveTreeManifest's lines without the save slots (any path with a slot folder in it): the host's files the game does
// not write as saves (profile, settings). Pure. M1.8: hashed before and after a host's game.
std::string NonSlotManifest(const std::string& manifest);

// "relative\path\tsize\tSHA-256" for every file below root, sorted ("" for a missing root).
std::string SaveTreeManifest(const std::wstring& root);
// Lines that differ between two manifests ("changed|added|removed <path>").
std::vector<std::string> ManifestChanges(const std::string& before, const std::string& after);

// The local two-instance test: low-quality windowed graphics for the sandbox's settings.cfg
// (sandbox.hpp) merged into `current` (the file as the game last wrote it; "" when missing):
// windowed, 1280x720, low quality. Written before every local test.
std::string LocalTestSettingsCfg(const std::string& current);

}  // namespace st::core
