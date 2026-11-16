Stranded Together @VERSION@ (build @BUILD_ID@)
Two-player online co-op for DEATH STRANDING DIRECTOR'S CUT (Steam).
A fan project, not affiliated with Kojima Productions, 505 Games or Sony.

WHAT YOU NEED
- DEATH STRANDING DIRECTOR'S CUT from Steam, Windows 10 or 11.
- Steam running and logged in (LAN mode and the solo tests work without it).
- The same Stranded Together version on both PCs.

INSTALL
1. Quit the game.
2. Open the game folder: in Steam, right-click DEATH STRANDING DIRECTOR'S
   CUT, then Manage > Browse local files. It is the folder with ds.exe.
3. Extract everything from this zip into that folder: StrandedTogether.exe,
   winhttp.dll and the stranded_together folder go next to ds.exe.
   Replace the files if Windows asks.
4. Start StrandedTogether.exe from the game folder.

The launcher checks its folder, the game version and that winhttp.dll is the
one from this zip (stranded_together\package.json). If another program's
winhttp.dll is already there, it says so and never replaces it.

Starting the game from Steam on its own runs it unmodded: co-op is on only
for a game started from StrandedTogether.exe (each start writes a one-shot
stranded_together\session.ini that the mod uses up).

PLAY
- Host: Play > Host > Host game. The game starts through Steam and a join
  code is copied to your clipboard. Send it to your partner.
- Join: Play > Join, paste the code, Join game.
- Solo test: Play > Solo test starts the game with an "echo", a copy of your
  character that replays your movement. No partner, no network.
- Keep the launcher open to see the connection (In game page). If you close
  it, a hidden helper switches co-op off after the game exits.

DEBUG
- Debug page > Options: start the debug tools on their own (no partner, no
  network) or the echo test; echo delay and offset, send rate, interpolation
  delay, waking the partner clone, verbose logging, and the in-game debug
  menu with its key (F9 by default). The options go into every game you
  start from the launcher.
- Debug page > Files and log: open the mod folder or log, a live view of
  stranded_together\stranded_together.log with Copy, Copy error.txt (the
  game's crash report), Disable / Enable mod (renames winhttp.dll to
  winhttp.dll.disabled and back) and Clear session (deletes session.ini).
- To report a problem: Tools > Collect logs puts a zip on your Desktop.
  Join codes, secrets and Steam IDs are removed.

UPDATES
The launcher checks GitHub for a newer release each time it starts (switch
this off on the About page). When one is out, press Update now: it downloads
the new zip, checks it, swaps the files and restarts the launcher. Close the
game first. Releases: https://github.com/TrollTaylor/stranded-together/releases

UNINSTALL
Quit the game, then delete StrandedTogether.exe, winhttp.dll and the
stranded_together folder from the game folder (or use Debug > Disable mod to
keep the files but play unmodded).

LICENCES
stranded_together\THIRD_PARTY_NOTICES.txt: MinHook (BSD 2-clause) and
Dear ImGui (MIT).
