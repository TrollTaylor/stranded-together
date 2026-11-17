<p align="center">
  <img src="docs/readme/logo.png" alt="Stranded Together" width="420">
</p>

<p align="center">
  <b>Two-player online co-op for DEATH STRANDING DIRECTOR'S CUT</b><br>
  Windows · Steam · <a href="https://github.com/TrollTaylor/stranded-together/releases">Releases</a>
</p>

Built on the launcher and network stack of
[Diamond Dogs Together](https://github.com/TrollTaylor/diamond-dogs-together) (MGSV co-op).
Not affiliated with Kojima Productions, 505 Games or Sony Interactive Entertainment.

## Milestone 1: two players, synced

- Two players connect over Steam (Steam Networking Messages, relayed, no port forwarding)
  with a join code `DST1-XXXXX-XXXXX-XXXXX-XXXXX-CC@<host SteamID64>`.
- Each player sees the other in their world, moved to the partner's position and facing
  at 30 Hz with 100 ms interpolation.
- **Experimental:** the partner is a solid character that copies the other player's real
  animation (walking, running, stopping). The guest picks which character the host sees;
  the host is always Sam. A partner shown as Sam wears the other player's own suit and colour
  (porter, silver or gold). A guest who picked another character appears as it on their own
  screen too. If the real character cannot be shown, the partner appears as a hologram
  instead. See [docs/real-partner-body.md](docs/real-partner-body.md).
- The partner's name floats above their head (your co-op name, or your Steam name).
- Players cannot walk through each other: on foot, your partner's body stops you like
  another character does.
- **Experimental:** the backpack and the delivery cases each player carries show on their
  character on the other screen, and on your own character when you appear as one.
- Not yet: equipment and weapons on the backpack, cargo colours, the Odradek, facial
  animation, vehicles, world/BT/mission sync.

## Download

Get `StrandedTogether-<version>.zip` from [Releases](https://github.com/TrollTaylor/stranded-together/releases). Both players need the **same version**.

The launcher updates itself: when a new version is out, Play shows **Update now**, which downloads it, checks it, installs it and restarts the launcher (not while the game runs). **About → Check for updates** looks again; the check when the launcher opens can be switched off there. The launcher's only network access is that check against this repository's releases and, on **Update now**, the download of the new zip.

## Install

1. In Steam, right-click DEATH STRANDING DIRECTOR'S CUT and choose **Manage → Browse local files**. This opens the game folder, the one with `ds.exe`.
2. Extract **everything** from `StrandedTogether-<version>.zip` into that folder: `StrandedTogether.exe`, `winhttp.dll` and the `stranded_together` folder.
3. Run **StrandedTogether.exe**.

The launcher checks that it sits next to `ds.exe` (otherwise it shows where Steam has the game, with **Open my game folder**), the game version, and that `winhttp.dll` is the one from the zip (`stranded_together\package.json`). Another program's `winhttp.dll` is reported and never replaced.

Starting the game from Steam on its own runs it unmodded: each start from the launcher writes a one-shot `stranded_together\session.ini` that the mod uses up.

## Play

1. **Host:** **Play → Host → Host game**. The game starts through Steam and a join code is copied to your clipboard; send it to your partner. The host always appears as Sam.
2. **Join:** **Play → Join**, paste the code, pick **Your character** (Sam, Fragile, Die-Hardman, Cliff, Deadman, Higgs and others), **Join game**. The host sees you as that character when both games use the real character (Debug → Partner body), and on your own screen you appear as it too (Debug → See yourself as your character). **Advanced** switches to LAN / VPN (UDP) and sets your name.
3. **Solo test:** **Play → Solo test** starts the game with an echo, a copy of you that replays your movement. No partner, no network.

Keep the launcher open to see the connection on **In game**; closing it is safe (a hidden helper tidies up after the game exits).

## Debug

The launcher's **Debug** page:

- **Options:** **Debug tools only (solo)** starts the game with just the in-game debug menu and logging (no partner, no network), or the **Echo test**; echo delay and offset, send rate, interpolation delay, waking the partner clone, verbose logging, the **in-game debug menu** on/off with its key (**F9** by default, F1–F12), and **Partner body**: **Real character (experimental)** (default) or **Hologram**: how your partner is drawn on your screen, and with Hologram your partner sees you as a hologram too (a hologram game does not send your character). These options go into every game started from the launcher.
- **Files and log:** open the mod folder or log, a live tail of `stranded_together.log` with **Copy log**, **Copy error.txt** (the game's crash report), **Disable mod / Enable mod** (renames `winhttp.dll` ↔ `winhttp.dll.disabled`) and **Clear session** (deletes `session.ini`).
- **Tools → Collect logs** puts a zip for a bug report on the Desktop (codes, secrets and Steam IDs removed).

## Uninstall

Quit the game and delete `StrandedTogether.exe`, `winhttp.dll` and the `stranded_together` folder from the game folder (or **Debug → Disable mod** to keep the files and play unmodded).

## How it works

| Piece | Where | Notes |
|---|---|---|
| Loader | `native/mod/dllmain.cpp` → `winhttp.dll` | ds.exe imports winhttp; the proxy forwards all 92 exports to System32 and starts the mod on its own thread. Does nothing unless `stranded_together\session.ini` exists. |
| Engine bindings | `native/mod/game_api.*` | Local player, Get/SetWorldTransform, EntityResource::CreateEntity, Entity::Remove — found by RE, located by signature. See [docs/reverse-engineering.md](docs/reverse-engineering.md). |
| Frame hook | `dllmain.cpp` | MinHook on the game's `SteamAPI_RunCallbacks` wrapper (main loop). |
| Sync | `native/mod/player_sync.*` | Sends local pose, interpolates the partner (position lerp + rotation slerp), spawns the partner from the local player's own EntityResource. |
| Link | `native/mod/link.*` + `native/net/*` | DDT's link v2 (HMAC-authenticated handshake, keepalive, RTT) over Steam or UDP. |
| Steam | `native/mod/steam_owner.*` | Gets `SteamNetworkingMessages002` from steamclient through the game's old steam_api (same trick as DDT on MGSV). |
| Launcher | `native/launcher/*` → `StrandedTogether.exe` | DDT's launcher, rebranded: Host / Join / Solo test, Debug page, checks the install against `package.json`, writes a one-shot `session.ini`, starts DS through Steam, updates itself from GitHub Releases. See [docs/launcher.md](docs/launcher.md). |

## Build

Visual Studio 2022 Build Tools (or newer) and CMake 3.20+.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

Outputs: `build/Release/winhttp.dll` (the mod), `build/Release/StrandedTogether.exe`
(launcher), `build/Release/st_fakepeer.exe` (test partner).

Release zip: `.\tools\package.ps1` builds and writes `dist\StrandedTogether-<version>.zip`
(see [docs/packaging.md](docs/packaging.md)); `.\tools\deploy.ps1 -Package` installs it into
the game folder for a test. A release (version bump, clean build, all tests, zip, tag, GitHub
release that the launchers offer): `.\tools\release.ps1 -DryRun`, then `.\tools\release.ps1`
(see [docs/releasing.md](docs/releasing.md)).

## Testing (see [docs/testing.md](docs/testing.md))

1. **Solo, no network** — `tools\deploy.ps1 -Session echo`, start DS from Steam, load a save.
   A second Sam should follow your path 2 s behind you, 1.5 m to the side.
2. **One PC, real network path** — `tools\deploy.ps1 -Session udp-host`, start DS, then run
   `build\Release\st_fakepeer.exe --role guest --local 27556 --remote 27555`.
   The fake partner circles you.
3. **Two PCs over Steam** — both put `StrandedTogether.exe` + `winhttp.dll` in the game folder;
   one clicks Host and sends the code, the other pastes it under Join.

In game: **F6** logs both positions to `stranded_together\stranded_together.log`, **F7**
respawns the partner. `tools\deploy.ps1 -Uninstall` removes the DLL again.
