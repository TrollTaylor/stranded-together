# The host-held save session (M1)

"The host holds the save, the guest never has the save." The host's game plays on the host's own saves; the guest's
game runs on a private folder, waits on its own title screen, whose menu then reads only "Waiting for <host> to start
the game" and Quit Game, until the host picks Continue, Load or New Game on its title, then loads the copy of the host's save that the link brings. Design:
milestone M1, decisions D1-1..D1-9 at their defaults (the host plays on its real saves; a backup of them before each
session, 5 kept; a New Game guest waits for the host's first save; the guest follows the host to the title; a 60 s
link-loss timeout; guest settings kept in the co-op folder only; late join = the host's newest save plus a one-time
"Teleport to host"; guest online features unchanged and unverified; guest slots wiped at the next session start).

Switch: launcher Debug > **Host holds the save** (default on; off = the old mode, each player on their own save,
which the existing scenarios and the v0.14 vehicle copies still use). session.ini `host_save=1`. Everything below is
off without it.

**Status (2026-11-19, after the hidden runs):** launcher, DllMain rule, transfer protocol, link channel 4 (stage 1) and the
game side (stage 2) are in, and every M1 scenario passes hidden (section 9): the waiting screen, the gate (New Game and
Continue refused at the title), the host's title Continue, in-world Load, Retry (detection only, see 9), New Game
(first save stood in for, see 9), quit to title (the guest follows), a 20 s and a 75 s link loss, late join with the
teleport offer, the version refusal and the DllMain refusal. The hidden runs found and fixed four bugs (section 9).

**2026-11-20, the guest's title in the game's own menu:** the play test of v0.18.0 never showed the waiting screen (the
launcher's Local test wrote no `host_save`). The guest's title now shows the session in the game's own title menu
(section 5 "The guest's title screen"): proven hidden, also on the launcher's own Local test files (section 9).

## 1. For players

* **Host:** play as in single player. Co-op progress is saved into your own save by the game. The launcher backs up
  your saves before each co-op session (`stranded_together\backups`, newest 5). Your screen says "Partner joining:
  sending your save", "Your partner joins at your first save" (New Game) and "<partner> is in your world".
* **Guest:** your game never opens your own saves. Its title screen is the game's own, but the menu has only two
  lines: **Waiting for <host> to start the game** and **Quit Game**. The first line follows the session: "Connecting to
  <host>...", "Waiting while <host> loads the game", "Waiting for <host>'s first save" (a New Game), "Receiving
  <host>'s save: 45%", "Loading <host>'s save...", "Reconnecting to <host>...", "Game versions differ: update both
  games" (the two builds in a banner above it). Selecting it does nothing; Quit Game closes the game as always.
  Continue, Load Game, Options, Update Overview and Open Source Licenses are hidden while you wait. During the boot
  screens, and when the game's menu cannot be used (another game build), a full-screen panel with the same information
  and **Leave** stands in (only pad A, Enter and Space reach the game behind it, for notices).
* **The guest's progress is not kept:** the guest plays a disposable copy of the host's world (orders, deliveries,
  grade, likes, equipment, materials, structures, story flags: gone at the host's next load and at the session's end).
  The guest spawns as a copy of the host's Sam at save time. Settings and controls changed during co-op stay in the
  co-op folder for the next co-op session. The guest's real saves are never touched.
* **Events:** the host loads another save (title or pause menu): the guest loads it too. Retry after a Game Over: the
  guest loads the host's newest autosave of the current world, or the save that world was loaded from when none was
  written yet (it stands in for the retry point and can differ slightly). The host quits to
  the title: the guest goes back to its title and waits (if the game's QuitGame cannot get it there, it stays in the
  world with "<host> is at the title screen"). A lost link: "Connection to <host> lost. Reconnecting..." in the world;
  after 60 s the guest goes back to the waiting screen and loads the host's newest save when the link is back. Joining
  while the host already plays: the host's newest save, then **Teleport to host** is offered once (F9 menu header).
* **Versions:** both games must be the same game version (build string, save version and flags). Otherwise both
  screens say "Game versions differ: <host> vs <guest>. Update both games." and nothing is loaded or written.
* **Unverified:** achievements, Steam stats and online features (likes, shared structures) on the guest's Steam account
  may still react to events in the host's world (D1-8).

## 2. Launcher (native/launcher/core) - stage 1

* **Host** (`RealModel::host`, `PrepareHostSave`): `host_save=1`, no sandbox. With Debug > **Back up my saves before
  co-op** (default on): `core::BackupHostSaves` copies the host's account folder (read only) to
  `stranded_together\backups\<yyyyMMdd-HHmmss>\DeathStrandingDC\<account>\` (`.partial` until complete, `backup.json`
  inside with a `seq`, the newest 5 by `seq` kept: never by the local-time name, never the backup just made).
  `NonSlotManifest(SaveTreeManifest(real))` before and after the game: `launcher.log` `host_save=checked nonslot_changed=N`.
* **Guest** (`RealModel::join`, `PrepareCoopGuest`): `core::SeedSessionGuest` prepares
  `stranded_together\sandbox\coop-guest\saves` (slots and `incoming\` wiped each session, another account's folder
  deleted, the guest's own `profile\` and settings copied when missing, the redirect probe last), then `host_save=1`,
  `sandbox_saves=coop-guest`, `sandbox_foreign_saves=1`; the real tree hashed before and compared after the game.
  Refused before anything is deleted when the folder, a folder above it or anything below `saves\` is a link (a
  junction from `DeathStrandingDC\<account>` to the real account folder would otherwise wipe the real slots; unit test
  with real junctions).
* **The writer's rule** (`core::MakeSessionIniText`): a guest with `host_save=1` needs `sandbox_saves=coop-guest` (or
  `local-guest`, below) and `sandbox_foreign_saves=1` in the same file; `host_save` for echo / solo is refused.
* **The local two-player test** (Debug > Test saves; v0.19, `core::MakeLocalTestInis`): with the option on (default)
  both games carry `host_save=1`, so the local test plays this session exactly as Host / Join do. The host game plays
  its `local-host` sandbox copy of the test save (never the real saves, so no backup and no non-slot check); the guest
  game runs on `local-guest`, prepared like `coop-guest` before every start (`SeedSessionGuest(..., local-guest)`: its
  slots and `incoming\` wiped, your profile copied when missing). v0.18.0 wrote no `host_save` in the local test: the
  user's play test had no waiting screen, no shared vehicles (the v0.14 see-through copies stood in), no riding and no
  hits, and nothing on screen said so. Every launcher path now: Host `host_save` = the option (backup only on the real
  saves); Join = the option (on coop-guest); the local test = the option (on local-host / local-guest); Host / Join with
  "Also for Host, Join, Solo test" = the option (the host on the test copy without a backup, the guest on coop-guest);
  `--write-session host|join|local-test` = the option.

### When the features are off

The session and everything on it (the waiting screen, the save transfer, the shared vehicles, riding together, vehicle
hits) need `host_save=1`. A game that has them off says so instead of quietly playing the old mode:

* the mod logs `[coop] host_save=0 (host|guest): the host-held save session is OFF, and with it ...`;
* with `host_save=0` for a host or a guest, an amber overlay banner `Co-op features OFF: host_save=0 (the launcher's
  Debug > Host holds the save is off): no waiting screen, no shared vehicles, no riding together, no vehicle hits (the
  partner's vehicles are the old copies)` at the title, while loading and for the first minute of each stay in the
  world (`coop::NoticeTick`; `[coop] notice shown|hidden`), and the menu header keeps `OFF: ...`; the overlay is
  installed for it even with `debug_menu=0` and `name_tags=0`;
* a session that is on but has a feature off (a byte check, `vehicle_sync`, `ride_together=0`,
  `partner_vehicle_damage=0`) keeps its own one-minute notices (`SetVehicleNotice`);
* every case: `link-status.json` `"features_off"` (`player_sync.cpp FeaturesOff`), which the launcher shows on the
  Play page, the In game page and the local test card.

## 3. The mod's DllMain rule (native/mod/sandbox.hpp, automation.cpp) - stage 1

`role=guest` with `host_save=1` and no save redirect requested: `sandbox::Decide` answers `Mode::refuse_host_save`;
ds.exe ends in DllMain with exit code `0x53540006` and `<mod>\sandbox-refused.txt`. A ds.exe started by the hidden-test
supervisor gets this same refusal (the supervisor's own "no automation=1" refusal no longer comes first for it), which
`guest-refuse-unredirected` checks. `automation::Init` checks again later, and `coop::Install` last (a host-save guest
without the redirect ends there with the same exit code; the session is never just switched off). Every reader of
session.ini (DllMain's `EarlyInit`, `automation::Init`, `LoadConfig`) strips a UTF-8 BOM from the first line
(`automation::detail::StripBom`), so a file saved "UTF-8 with BOM" with `role=guest` first reads the same everywhere.

## 4. STX1, the save transfer (native/mod/save_xfer.*) - stage 1

Reliable ordered link channel 4. Every message: `u8 0x53 | u8 1 | u8 kind | u8 flags 0 | u32 xfer_id`, then:

| Kind | Direction | Body | Bytes |
|---|---|---|---|
| 1 STATE | host->guest (and guest->host, see below) | u8 phase (0 title, 1 loading, 2 world, 3 quitting), u32 load_counter, u8[32] current_save_sha, u32 offer_xfer_id, char[24] game_build, u32 savegame_version, u32 save_flags, u8 new_game_pending | 8+74 |
| 2 GSTATE | guest->host | u8 phase (0 waiting, 1 receiving, 2 loading, 3 world, 4 refused), u32 loaded_load_counter, u8[32] loaded_sha, u32 have_bytes, u16 refuse_reason | 8+43 |
| 3 OFFER | host->guest | u32 load_counter, u8 slot_type, i8 slot_index, char[24] slot_name, u32 size, u8[32] sha256, u8 approx, u8 reason (0 title-continue, 1 title-load, 2 new-game-first-save, 3 in-world-load, 4 retry, 5 late-join), game_build, savegame_version, save_flags | 8+100 |
| 4 WANT | guest->host | u32 next_offset, u8[32] sha256 | 8+36 |
| 5 CHUNK | host->guest | u32 offset, u16 len (1..1000), data | 8+6+len |
| 6 END | host->guest | u32 size, u8[32] sha256 | 8+36 |
| 7 RESULT | guest->host | u8 ok, u16 code (0 ok, 1 sha mismatch, 2 parse, 3 version, 4 disk, 5 load refused, 6 load failed) | 8+3 |
| 8 ABORT | either | u16 code (1 superseded, 2 cancelled, 3 read failed, 4 protocol, 5 session end) | 8+2 |

The design said 8+80 / 8+44 for STATE / GSTATE; its fields add up to 74 / 43, which is what is sent. **Stage 2
addition:** the guest also sends a STATE (phase 0, everything else zero) carrying only its own game version, at once
on each link session and every 5 s: GSTATE has no version field, and the host needs the guest's build to say "Game
versions differ: <host> vs <guest>" and to stop offering.

`Sender` (host) and `Receiver` (guest) are pure state machines (unit-tested, `st_save_xfer_tests`). The Receiver loads
one save at a time: a save that completes while the game still loads the previous one waits (`TakeComplete` refuses
until that load's `LoadDone`), so each RESULT names the transfer the game really loaded. On a new link generation
the guest calls `Receiver::NewSession()`: the running transfer is forgotten (its part's bytes stay, so the same file
resumes), a running load and the loaded save are kept; a restarted host's offers (load counter and xfer id from 1
again) are taken instead of being dropped as older. Host reads go through `ReadSaveFileReadOnly` / `ReadSaveSnapshot`
(GENERIC_READ and full sharing only, asserted; the handle closed right after each read, never held across a wait).
Full sharing does not protect the game's own save: it writes `checkpoint.dattemp` and replaces `checkpoint.dat` with
one `MoveFileExW(..., REPLACE_EXISTING | COPY_ALLOWED | WRITE_THROUGH)` (0x141A01ED0 -> 0x14192AEA0 -> 0x14192B170, no
retry; on failure the temp is deleted and that save is lost), which fails with access denied while any handle to the
target is open. So the reader opens nothing while a `checkpoint.dattemp` is next to the slot (status `busy`, retried),
and the host waits for a slot without one. The window that is left is the few milliseconds of one read. Guest writes go through `WriteGuestSaveFile` / `SandboxPartStore` (only below
`automation::SavesScratchRoot()`, only `incoming\<sha>.part` and `DeathStrandingDC\<account>\manualsave<N>\checkpoint.dat`,
temp file + `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)` + read-back SHA).

`ValidateCheckpoint` reads each of the three 8-byte size fields as decimal digits ended by a NUL; the bytes after the NUL
are not checked: the game leaves its buffer's leftovers there in the saves it writes itself (run 20261119-074751: a
fresh autosave began `410\0p\x05\0\0`; the seed saves happen to be zero-padded).

## 5. The game side (native/mod/coop_session.*) - stage 2

`coop::Install` runs on the bootstrap thread right after `LoadConfig`, before the overlay waits for the game's
d3d12, so the guest's title is gated before it can ever be shown. It byte-checks every address (section 6), installs
the hooks (MinHook) and starts one worker thread. `coop::Tick` runs on the game thread from `sync::Tick`.

**Threads.** Hooks and `Tick` (game thread) only record what happened and make the game calls; the worker thread owns
the `Sender` / `Receiver`, all file I/O (the host's read-only snapshot, the guest's part file and slot write) and the
link's channel 4 (`link::PopSave` / `SendSave`, flow control `SaveQueued() < 32`). One SRW lock guards what both see.

**Host detection** (detours, game thread, SEH around the game calls):

* `0x142624A70` title load (app, type, index): Continue arrives as (0xF, -1) -> reason title-continue, else title-load.
  The slot is named before the game's function runs, but the load is offered only when it returns non-zero (0 = the
  game refused it: the host stays at its title, `the title load returned 0: ... nothing is offered`).
* `0x142624C80` converted load (quicksave newest): reason title-load.
* `0x1426393E0` new world (New Game): `new_game_pending`; the worker notes every slot's write time and polls them read
  only (`GetFileAttributesEx`, opens nothing) at 1 Hz; the first new save, once it has not changed for 2 s, is offered
  (reason new-game-first-save).
* `0x14362D750` in-world load request: index >= -1 -> in-world Load (pause menu); index -2 -> Retry (the newest
  autosave, `approx=1`), **except** the game's own follow-up of a load: a request while the session global is 0, or
  within 10 s of a detected load before a new local player has been stable for 5 s (the -2 rule; the smoke run saw the
  follow-up 0.7 s after the title load).
* Each detected load gets the next `load_counter`; the slot is named with the game's slot-cache resolver
  `0x143635630` (string released with `0x14190AD20`).
* The host's phase (STATE): world = the session global `[0x147BE6318]` set and a local player stable for 5 s with no load
  pending; loading = a load pending or a session without a stable player; title = no session and no load pending
  (polled each tick: "back at the title").

**Host read** (worker): the account folder `%USERPROFILE%\AppData\Local\KojimaProductions\DeathStrandingDC\<SteamID & 0xFFFFFFFF>`.
The id is the one the game itself runs as, asked from the game's own steam_api64 on the game thread
(`steam::QuerySelfId`: SteamUser021 GetSteamID; works with transport=udp, where the Steam transport never starts;
`[coop] the game runs as Steam account N`). Until Steam answers: the one account folder there is; with several, nothing
is offered (`nothing is sent rather than another account's save`, log and host banner "Your save could not be sent to
..."): another account's save never leaves the PC. The guest writes into the same account the same way (else the
one folder the launcher seeded). Then: waits until the slot's write time is 2 s old and no `checkpoint.dattemp` is
there; `ReadSaveSnapshot` (two reads
400 ms apart, equal SHA-256, up to 3 tries); the write time must be unchanged after the read; `ValidateCheckpoint`; then
`Sender::Offer`. The link sends up to 16 reliable packets per tick while the host is not in the world
(`link::SetSaveBurst`), 4 in the world.

**Late join / rejoin** (worker, D1-7): when the guest's GSTATE says it waits (or is in an older world) while the host
is in the world and nothing is being sent, the host offers its newest save, reason late-join, at most once per 30 s.
"Newest" is always of the host's CURRENT world (`rules::NewestCandidates`): the host's mod records the slot each
title Continue / Load, in-world Load or New Game first save came from and when; only slots written since that load
count (newest first), and when none was written yet, that slot itself (`nothing written since this world's load: its
own slot, manualsave0`). An older world's newer-looking autosave is never offered for the world the host plays now
(the same for Retry's approx autosave: run `20261119-085501` puts the guest at the host's spot, not 2.2 m away). On a new link session an offer that was never answered (title Continue / Load / late
join) while the host is already in the world is cancelled as stale (the newest save follows); any other running
offer is announced again and the guest resumes it. The link keeps its session (and its generation) when the peer is back
within its reconnect window, so a link that is connected again after being down is handled the same way ("the link is
back (link session #N kept): handled as a new session"); without this, run 20261119-080005 delivered the stale Continue
offer to the late guest (and no teleport offer). "The newest save" (Retry, late join, offer now) tries the three newest
slots in turn when one cannot be read or validated.

**The guest's hard gate** (detours): `0x14327B1A0` (the title menu's start flow), `0x14324F4B0` (the load page's
decision), `0x142624A70` (title load, second layer), `0x142624C80` (converted load), `0x1426393E0` (new world) return
without acting ("[coop] gate: BLOCKED ...") unless the mod's own `allow` flag is set, which is set only around the mod's
own title-load call on the game thread.

**The guest's waiting screen** (overlay, `overlay::SetCoopView`): a full-screen opaque panel while the guest's session
global is 0 and no title load of ours has been accepted; the game's input is blocked as for the open menu
(`input::SetWaitBlock`: keyboard hook, window subclass, raw input, cursor, XInput) except pad A, Enter and Space; the
Leave button posts WM_CLOSE from the overlay itself (it works even when the game thread's tick is dead). The menu (F9)
still opens over it.

**The guest's title screen** (`native/mod/title_wait.*`): while the guest waits at its title, the game's own title menu
carries the session's state. Everything is done with the data writes and calls the game's title menu makes itself,
from MinHook post-hooks on its two per-frame updates, on the game's own menu thread (the game thread: the coop tick and
the hooks log the same thread id), after the original:

* `DSUITitleMenuDataSource::Update` (vt[5] `0x143273060`; vtable `0x143E94C98` checked on `this`): the data source is a
  table from key (crc32c of the binding's name) to value, which the view's bindings read. Through the game's own
  find-or-create `0x141B257D0(ds+0x20, ds+0x28, &key, 0)` and string assign `0x14191CBC0`, with its compare-then-write
  pattern (value, valid = 1, `ds+0x60` dirty): bool `0x7339FA72` ("has a save": CONTINUE and LOAD GAME) and bool
  `0x1A8722BC` (the "Load Game (Death Stranding Save Data)" item) := false; string `0x06F2B2AF` (NEW GAME's label,
  `txt_new_game` / `txt_new_game_bg`) := the label (`rules::TitleLabel`, at most 46 characters; the host's name only as
  printable ASCII up to 16 characters, else "the host": the game font may lack the glyphs). The game rewrites these only
  on its own flags (`ds+0x88`, the title state's `+0x81`), and the post-hook writes again in the same call.
* `DSUITitleMenuFunction::Update(this, dt)` (vt[14] `0x143278AB0`; vtable `0x143E94BE8` checked): the items with no
  Visible binding are hidden with the game's `SetVisible 0x141B56850(tag, 0)` (as the game hides its own panels), found
  by element name: `this+0x50` (resource) `+0x70` (lst_title_menu_list's resource, its name checked) `+0x180`
  ChildElements, each item's name at `+0x20`; the tag by `FindTagByResource 0x141B1DE10(menu system, item, 0)` (as Init
  and Update do), kept only when it is a child of the list's tag (`this+0xE0`: `+0x20` count, `+0x28` children).
  Hidden: everything but itm_new_game, itm_exit_game_menu and the three the bools hide (`rules::TitleItemHidden`:
  itm_config_menu, itm_update_info, itm_credit_menu; an item a later build adds too). Every read goes through
  ReadProcessMemory; a layout that does not match leaves the items alone (the label still works).
* `OnNewGame 0x1432799F0`, `OnContinueGame 0x1432778F0` and `OnSelectLoadSlot 0x14327B010` return at once while the
  guest waits (`[title] the label (NEW GAME) pressed on the waiting title: ignored #n`): no "start a new game?" dialog,
  no gate block. `OnExitGame 0x143277AD0` is untouched: Quit Game quits (no confirmation; run 20261120-031529: the
  guest's game exited with code 0 about 4 s after A).
* The full-screen panel (overlay) covers the boot screens and goes once the label is in the current page's data source
  and the menu's update ran within 1.5 s (`title_wait::Showing`); the game's input is then not blocked. From the end of
  the transfer ("Loading <host>'s save...") the label stays through the guest's own title load until the world is there
  (the page fades out under it). The gate (above) stays the hard layer.
* Back to the game's menu (the session stops, the guest is in the world, hidden-test op `8 3`): at the menu's next
  update the bools and the label are written back, `ds+0x88 = 1` (the game asks "has a save" again), the title state's
  `+0x81 = 1` (`*(*(u64*)0x147C00128 + 0x2D0)`: the game rewrites all 8 labels from its localized texts), and the items
  this page hid are made visible (only those still children of its list). A new title page (after QuitGame) is new
  objects: nothing of the old page is touched.
* Never a cached tag by its address alone: the items are hidden only while each tag is still a child of the page's list
  (read fresh from the vtable-checked menu every update), and when the menu had no update for over 1 s at the same
  addresses (a new page may sit in the old one's memory) its items are resolved again (`[title] the title menu had no
  update for N s at the same address: its items are resolved again (k of n kept, still in the list)`); only tags still
  in the list keep their "hidden by us" mark. The data source's entries are looked up by key on every write.
* Not with the hidden-run virtual-pad fallback (it presses CONTINUE on the game's menu) and never on the host: the
  host's title is the game's own.

**When the tick dies** (`coop::EmergencyStop`): the gate, the waiting screen and its input block live on only through
`coop::Tick`. When the per-frame tick faults (dllmain `GuardedEmergencyStop`) or Bootstrap stops before the frame pump
is hooked (game functions not found, MinHook, the pump hook), the session is switched OFF: the gate opens, the worker
idles, the waiting screen and the input block go, the game's own title menu comes back (title_wait), an amber banner says "Co-op save session stopped: <reason>", one
`[coop] host-held save session STOPPED` line.

**The guest's receive and load:** the Receiver writes `incoming\<sha>.part`; at END the SHA-256 and `ValidateCheckpoint`;
the worker writes `manualsave5`, the next one `manualsave6`, alternating (never the slot just loaded, never an autosave);
then the game thread: at the title only once the title menu can be up (`rules::TitleReady`: the game has no ready
signal the mod reads, so 25 s after the mod's first tick, ~30 s after the start, past the boot logos, "press any button"
and notices, and the session global 0 for 8 s without a break, past the world's teardown after QuitGame; until then
the save waits, `the title load waits until the title menu has been up for a while`), `0x141A00DA0(0)` slot-cache
refresh, `0x1426376E0(&app, 1, K)` exists check, 200 ms later the load: at the title `0x142624A70(&app, 1, K)` through its trampoline with the gate open for that call; in the world
`0x142636990(&app, 1, K)` (the pause menu's Load). Done when a new local player entity has been stable for 5 s
(RESULT ok), after 150 s RESULT load_failed. Fallback when the load calls fail their byte check: in hidden runs only,
a virtual pad A on CONTINUE with the gate open for 20 s (the session's slots were wiped at its start, so CONTINUE is
the save just written). Outside hidden runs the load calls are required for the guest: a failed check there turns the
session off at start-up (`save load call byte check failed`, the banner), never a waiting screen that can never load.

**Session events (guest):**

| Event | What the guest does |
|---|---|
| The host's STATE says title for 3 s while the guest is in the world | QuitGame `0x142642B20` (= `0x142635B50([session], 1, 0)`); the session global must reach 0 within 30 s ("back at the title": the waiting screen again); otherwise it stays in the world with "<host> is at the title screen" (D1-4; which one happens is decided by a hidden run) |
| The link is lost while in the world | banner "Connection to <host> lost. Reconnecting... (back to the waiting screen in N s)"; after 60 s QuitGame as above (D1-5); when the link is back the host compares and offers again if needed |
| A late-join load completes | "Teleport to host" offered for 90 s, once (menu header button, or `action coop_session 3`): the Teleport to partner action |
| A version difference (STATE / OFFER) | GSTATE refused (reason 3), RESULT version for any offer, nothing stored or written; both screens show the two builds |

**Versions** (game thread, first tick): the build string from `0x141A13610` (a Decima string; its first two words, e.g.
`dsq 179/4027081`), the SaveGame version `0x143635620` (67) and the save flags `0x1436355A0(0)`. `coop_test_build=<text>`
(hidden runs only) fakes the build for the version scenario.

## 6. Byte checks (coop_session.cpp `kSites`)

| Address | What | Bytes checked | Needed by |
|---|---|---|---|
| 0x142642A00 | ContinueLastSave (its operands name the session `0x147BE6318` and app `0x147BE6330` globals, checked) | `48 83 3D 10 39 5A 05 00 75 11 41 B0 FF 48 8D 0D 1C 39 5A 05 B2 0F E9 55 20 FE FF C3` | both |
| 0x141A13610 | game build string | `40 53 48 83 EC 50 65 48 8B 04 25 58 00 00 00 8B 0D` | both |
| 0x143635620 | SaveGame version | `B0 43 C3` | both |
| 0x1436355A0 | save flags | `48 89 5C 24 08 57 48 83 EC 20 0F B6 D9 33 FF E8` | both |
| 0x142624A70 | title load | `40 56 41 54 41 57 48 81 EC 90 00 00 00 48 8B 05` | host detection; guest load call |
| 0x142624C80 | converted load | `40 56 48 81 EC 80 00 00 00 48 8B 05 A0 C3 93 02` | host detection; guest gate |
| 0x1426393E0 | new world (New Game) | `48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 80 B9` | host detection; guest gate |
| 0x14362D750 | in-world load request | `48 89 5C 24 20 88 54 24 10 55 57 41 54 41 55 41 56 48 83 EC 30` | host detection |
| 0x143635630 | slot name resolver | `40 55 53 56 57 41 55 41 56 41 57 48 8D 6C 24 F9 48 81 EC A0 00 00 00` | host detection |
| 0x14190AD20 | Decima string release | `40 53 48 83 EC 20 48 8B 19 48 8D 05` | host detection |
| 0x14327B1A0 | title menu start flow (OnStartGameFlowImpl) | `40 53 48 83 EC 20 48 8B D9 E8 F2 73 D9 FF BA 12 75 ED 63` | guest gate |
| 0x14324F4B0 | load page decision | `40 53 48 83 EC 20 48 8B D9 48 8B 89 B8 00 00 00` | guest gate |
| 0x141A00DA0 | slot cache refresh | `4C 8B DC 55 49 8D AB 28 FE FF FF 48 81 EC D0 02 00 00` | guest refresh |
| 0x1426376E0 | slot exists | `48 83 EC 68 48 8B 05 45 99 92 02 48 33 C4` | guest load call |
| 0x142636990 | pause-menu Load | `48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 40 41 0F B6 F0 0F B6 EA` | guest load call; host test op 5 |
| 0x142642B20 | QuitGame (its operand names the session global, checked) | `48 8B 0D F1 37 5A 05 45 33 C0 41 8D 50 01 E9 1D 30 FF FF` | guest follows to the title; test op 6 |
| 0x14362B4A0 | a checkpoint exists (session+0x2A0) | `48 8B 81 C8 00 00 00 48 85 C0 74 19 80 78 24 00 75 10 66 83` | test op 7 only |

The session needs "both" plus host detection (host) or the gate, the refresh and (outside hidden runs) the load calls
(guest). A failure there turns the session off for the run: `[coop] host-held save session OFF (...): this game build
differs (... byte check failed)` and a banner "Co-op save session off: ..." for a minute (then in the F9 menu header).
The guest's load calls in hidden runs, QuitGame and the test guard are optional (fallback / stay in the world / test op
refused). The Steam calls of `steam::QuerySelfId` are steam_api64 exports by name (GetProcAddress), not game addresses. The smoke run: `17 byte checks, all ok`
on both games (the test guard was added after it).

**The guest's title screen** (title_wait.cpp `kSites`, 13 checks, plus the vtable slots DS vt[5] = `0x143273060`,
TMF vt[13] = `0x143278430` and vt[14] = `0x143278AB0`, and the two globals through the instructions that name them). A
mismatch leaves it off (`[title] byte check FAILED: ...`, `[title] the guest's title in the game's own menu is OFF
(...): the full-screen waiting panel is used`; the panel's last line says so too); the session itself is not affected.

| Address | What | Bytes checked |
|---|---|---|
| 0x143273060 | DSUITitleMenuDataSource::Update | `48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 A0 FC FF FF` |
| 0x143278AB0 | DSUITitleMenuFunction::Update | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 40 C5 F8 29 74 24 30` |
| 0x143278430 | DSUITitleMenuFunction::Init (the field map) | `48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B D9 E8 54 A1 D9 FF` |
| 0x143278469 | in Init: names the menu system `0x1457965F8` (checked) | `48 8B 0D 88 E1 51 02 48 8B 55 30 E8` |
| 0x141B257D0 | data source find-or-create | `48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 49 8B F0 48 8B FA 48 8B E9` |
| 0x14191CBC0 | Decima String assign from char* | `40 53 48 83 EC 20 48 8B D9 49 C7 C0 FF FF FF FF 49 FF C0 42 80 3C 02 00` |
| 0x141B56850 | Tag SetVisible | `3A 91 B0 00 00 00 74 1F 48 8B 05 99 FD C3 03 4C 8B 80 B8 00 00 00` |
| 0x141B1DE10 | FindTagByResource | `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 49` |
| 0x1432778F0 | OnContinueGame | `40 53 48 83 EC 20 48 8B 91 08 01 00 00 48 8B D9 E8 6B CF 00 00` |
| 0x1432799F0 | OnNewGame | `40 53 48 83 EC 20 48 8B D9 E8 A2 8B D9 FF BA 03 00 00 00 B9 01 00 00 00` |
| 0x14327B010 | OnSelectLoadSlot | `48 83 EC 28 48 8B 91 18 01 00 00 E8 50 98 00 00 E8 3B 05 FD FF C6 40 30` |
| 0x143277AD0 | OnExitGame (not hooked: Quit Game must quit) | `48 83 EC 28 66 C7 05 7B E8 96 04 01 01 E8 BE AA D9 FF` |
| 0x1430125B9 | in GetUIGlobals: names the UI globals `0x147C00128` (checked; `+0x2D0` = the title state) | `48 8B 05 68 DB BE 04 48 85 C0 75 1B` |

## 7. Logs, debug action, overlay

Every step is one `[coop]` line: install and byte checks, each detected load (`host: title Continue (type 0xF, index
-1) -> manualsave0 (load #1)`), the follow-up rule, the read (`bytes, sha256, two equal reads in N s`), each offer
(xfer, slot, reason, bytes, sha), WANT / resumes, the transfer's end (chunks, bytes, seconds), the partner's RESULT
(with chunks, bytes, seconds, resumes, restarts, link retransmits for that offer), the guest's receive (`received N bytes
in N s (KB/s), sha256 equal ... dropped chunks, resumes, sha retries`), its write, refresh, load call and "in the world
after N s", gate blocks (the first 20, then every 50th), phases, link loss and return, QuitGame, version differences.
The guest's title: `[title]` lines (hooks in, or OFF with the reason; `lst_title_menu_list (title page #n): 8 items
(...); 3 to hide`; `native waiting title ON (title page #n)`; each new label, the transfer's percentage at most once a
second; presses ignored; `the game's own title menu is back`). `coop_session 0` on the guest adds `[coop] status:
native title installed; wanted 1, label in the menu 1, menu update 0.0 s ago, items hidden 3, presses ignored 0, faults 0`.

Debug action **Coop session** (`action coop_session <op> [a b]`): 0 status; 1 `<ms>` drop every received datagram for
ms (hidden runs only: the link-loss and late-join scenarios use it on both games); 2 offer the newest save of the
current world now (host);
3 accept the teleport offer (guest); 4 log the slots (host); 5 `<type> <index>` the pause menu's Load on the host
(hidden runs); 6 QuitGame on this game (hidden runs); 7 the checkpoint retry request as the Game Over screen's caller
`0x142DCAC60` makes it: `0x14362D750(session+0x2A0, 4, -2, 2, 5, -1, 1)` while `0x14362B4A0` says a checkpoint exists
(host, hidden runs; the -1 argument is a guess, the caller passes its own); `7 1` only asks whether a checkpoint exists
(always ok, for polling); `7 2` makes the request even without one (the game itself ignores a -2 / reason 5 request
without a checkpoint, so only the host's detection sees it); 8 `<0|1|2|3>` hide the waiting panel for a screenshot of
the title behind it (guest, hidden runs; 1: the input block and the gate stay; 2: the input block is lifted too; 3: the
game's own title menu comes back as well (title_wait restores it), so the gate alone holds the title; 0: all as before); 9 `<type> <index>` a slot stands in for a New Game's first save: it is dropped from the New Game
baseline, so the read-only 1 Hz poll offers it (host, hidden runs; nothing is written).

The overlay draws the co-op screens only with its swapchain-creation hooks (like the name tags); with a host-save
session the overlay is installed even with `debug_menu=0`.

## 8. Hidden tests (tools/testenv/scenarios)

All with `Start-TestRun.ps1 -Scenario <name> -SeedSaves complete-100 -GuestSeed profile-only` (the guest's scratch has
no slot; the host's has the 100% save; the `launcher` scenarios without `-GuestSeed`: the launcher's local-guest folder
is profile-only already), foreground, one at a time:

| Scenario | Proves |
|---|---|
| `two-games-hostsave-title` | the guest's title in the game's own menu: only the label and Quit Game (`T1`), DOWN selects Quit Game (`T2`), A on the label ignored (no dialog, no gate block), a 12 s link loss ("Reconnecting to the host...", `T3b`), the transfer's percentage (`T4`) and "Loading the host's save..." (`T5`), then the world; the host's title the game's own (`T1-host-title`) |
| `two-games-launcher-local-title` | the same on the launcher's own Local test files (`launcherini local-test`, no `-GuestSeed`): the session ON from the launcher's `host_save=1`, the guest's native title, transfer and load |
| `two-games-hostsave-continue` | the guest's native title; A on the label ignored; with the game's own menu back for the test (`8 3`: every item and the "New Game" label restored, `02a`) the whole New Game path (OK, difficulty, OK, text size) ends at the gate (`gate: BLOCKED ... New Game`); the host's Continue over channel 4; the guest's title load from that state; both at the same spot (snapshots) |
| `two-games-hostsave-reload` | the host's in-world Load of autosave18 (`coop_session 5 4 18`) followed in the guest's world (manualsave6); then the host's QuitGame and the guest following it to the title (D1-4); its new title page (#2) hides CONTINUE / LOAD GAME although the guest now has manualsave5/6; A on the label ignored; Quit Game (DOWN, then `@guest quit pad`) ends the guest's game with exit code 0 |
| `two-games-hostsave-retry` | the -2 Retry request (`coop_session 7 2`: no checkpoint exists in a freshly loaded world, see 9), the approx newest-autosave offer, the guest's in-world load |
| `two-games-hostsave-newgame` | the host's New Game (one DOWN, OK, difficulty, OK, text size), the guest's "you join at their first save" state for 35 s, then autosave18 standing in for the first save (`coop_session 9 4 18`): offered (reason 2) and loaded at the guest's title |
| `two-games-hostsave-linkloss` | a 20 s outage (banner, no reload), then a 75 s outage (back to the waiting screen after 60 s, the newest save on return) |
| `two-games-hostsave-late` | the host in the world before the link exists; the stale Continue offer cancelled; late join with the newest save; the teleport offer accepted |
| `two-games-hostsave-version` | `@guest ini coop_test_build=...`: both sides report the difference (the guest's title label, a banner with both builds), RESULT version, nothing written |
| `guest-refuse-unredirected` | the supervisor's `guest unredirected` directive: the guest's session.ini has `host_save=1` and no redirect; exit `0x53540006` and `sandbox-refused.txt` within 15 s (summary `guest_refused_as_expected`) |

Checks on every run (supervisor): real tree unchanged, 0 cloud uploads, restore complete; the guest's scratch diff
should hold only `incoming\`, `manualsave5/6`, its own autosaves and `profile.dat`.

## 9. Hidden runs (2026-11-19) and what is not verified

**2026-11-20, the guest's title screen** (all `pass`: game and guest exit 0, real saves unchanged, restored, 0 cloud
uploads, no violation):

| Run | Scenario | Result |
|---|---|---|
| 20261120-031041 | two-games-hostsave-title | first build: label, items hidden, A ignored, transfer, load, world |
| 20261120-031529 | two-games-hostsave-reload | title page #2 after QuitGame (CONTINUE / LOAD GAME stay hidden with manualsave5/6), A ignored, Quit Game: guest exit 0 about 4 s after A |
| 20261120-031830 | two-games-hostsave-version | the label "Game versions differ: update both games", the banner with both builds |
| 20261120-032248 | two-games-hostsave-title | + "Reconnecting to the host..." during a 12 s link loss, "Waiting for ..." again after it |
| 20261120-032439 | two-games-hostsave-continue | found: after `8 3` the restored NEW GAME label was empty (the saved label had been taken before the game wrote its own); fixed with the title state's `+0x81` (the game rewrites its labels) |
| 20261120-032712 | two-games-hostsave-continue | final build: the restore shows every item and "New Game" (`02a`), the New Game path ends at the gate, Continue, world |
| 20261120-032917 | two-games-launcher-local-title | final build, on the launcher's Local test files (`host_save=1`, `second_instance=1`, `my_character=fragile_suit`): native title, transfer, "Loading" straight after the transfer, world |

Not verified for the title: a real mouse on the menu (the handlers are the same for a click); a non-English game
language (the label is our English text; the restored labels are the game's own); a host name other than "the host"
(the partner's name arrives with the pose frames, which the host sends only in the world, so at the title the label
says "the host"); the emergency stop's restore (the `8 3` restore uses the same path); a game notice (offline mode)
over the native title (none appeared in these runs).

All with `-SeedSaves complete-100 -GuestSeed profile-only` (the refusal without `-GuestSeed`), foreground, one at a time.
Every run: result pass, both games exit 0 (the refused guest: 0x53540006), real tree unchanged, restored, no violation,
0 cloud uploads; the host's scratch changed only by the game (profile.dat; its own autosave4 in the link-loss run); the
guest's scratch only by manualsave5/6, its own autosave0 and profile.dat (plus the empty slot folders the game creates).

| Scenario | Final run | Transfer | Position (host / guest snapshot) |
|---|---|---|---|
| continue | `20261119-081556` | 1,762,768 B in 4.2 s (408 KB/s, burst), RESULT 14.0 s after the offer; gate blocks 1 (New Game) | equal (-1100.18, 273.92, 352.49) |
| reload | `20261119-072416` | #1 439 KB/s; #2 (autosave18, in the world) 1,778,586 B in 4.2 s, RESULT 10.6 s; gate blocks 1 (Continue); QuitGame reached the title in 0.6 s | equal after both loads |
| retry | `20261119-073512` | autosave1 (approx) 1,767,433 B, RESULT 10.7 s | the host does not move (no checkpoint), the guest at autosave1: 2.2 m apart |
| newgame | `20261119-074529` | autosave18 as the first save, 191 KB/s (the host is "in the world" in the intro: no burst), RESULT 18.6 s | the guest at autosave18; the host in the intro |
| linkloss | `20261119-081221` | 20 s: banner, link back, no reload; 75 s: QuitGame after 60 s (title in 0.6 s), then late join of autosave4 (written by the host's game in the run), 191 KB/s | 0.64 m (the host moved after its autosave; teleport offered) |
| late | `20261119-080310` | the stale Continue offer cancelled on reconnect; manualsave0 for late join, 192 KB/s; teleport accepted | equal after the teleport |
| version | `20261119-080515` | RESULT version, 0 bytes sent, nothing written; both screens show the two builds | - |
| guest-refuse-unredirected | `20261119-080645` | the guest exits 0x53540006 after 0.09 s; `sandbox-refused.txt` names host_save | - |

Not-M1 regressions on the same build: `two-games-vehicle-state` (`20261119-080713`), `two-games` (`20261119-080949`),
`sandbox-alive -SandboxSession` (`20261119-081108`, ended by the supervisor as designed): pass.

**After the review fixes** (the account the game runs as, the current world's newest save, one load at a time, the
receiver's new link session, the title-ready wait, the offer after the title load returns, the reader's `.dattemp`
rule, the load calls required, the emergency stop, the BOM; same flags, foreground, one at a time), every run pass,
exit 0 / 0 (the refused guest 0x53540006), saves unchanged, restored, no violation, 0 cloud uploads:

| Scenario | Run | What the fixes changed in it |
|---|---|---|
| guest-refuse-unredirected | `20261119-085010` | as before (exit 0x53540006 after 0.09 s) |
| continue | `20261119-085035` | `the game runs as Steam account N` on both (transport udp); equal snapshots |
| late | `20261119-085258` | late join: `nothing written since this world's load: its own slot, manualsave0`; equal after the teleport |
| retry | `20261119-085501` | the approx offer is the world's own slot (manualsave0), not an older autosave: host and guest now at the same spot |
| linkloss | `20261119-085659` | rejoin: `the newest save of this world is autosave4`; title load 22 s after QuitGame reached the title |
| reload | `20261119-090027` | as before |
| newgame | `20261119-090241` | the baseline from the game's own account folder |
| version | `20261119-090457` | as before |
| two-games (old mode) | `20261119-090615` | regression: pass |

**Bugs the runs found (fixed):**

* A profile-only guest's title has NEW GAME selected; A opens "Starting a new game ... overwriting" with **Cancel**
  selected, then a difficulty page, a second confirmation (Cancel selected) and Change Text Size before the start flow
  (runs 20261119-071338, -071826). The old scenario's presses never reached the gate. The scenarios now walk it.
* The game writes its own saves with leftovers after the NUL of each size field: every in-session autosave of the host
  failed `ValidateCheckpoint` (`bad_size_field`), so a late join or a rejoin after a lost link offered nothing (run
  20261119-074751). Fixed in `SizeField`; unit test added.
* A link back after an outage keeps its link session: the coop layer never saw a new session, so the stale Continue
  offer went to a late guest (run 20261119-080005). Fixed (section 5).
* A newest save that cannot be read is now skipped for the next newest.
* Testenv: the guest's scratch diff used the host's "before" manifest (a profile-only guest showed every stripped slot as
  removed); the supervisor now writes `saves-guest-scratch-before.txt`.

**Infeasible in a hidden run, and what is proven instead:**

* **Retry with a real checkpoint.** Every load request except (-2, reason 5) clears the checkpoint record
  (0x14362D750: `memset(session+0x2A0+0xC8, 0, 0x200)`), so a freshly loaded world has none until play passes a
  checkpoint; runs 20261119-072858 and -073105 polled 0x14362B4A0 for over 3 minutes (standing and walking): absent.
  0x14362D750 with (-2, 5) checks 0x14362B4A0 itself and returns without acting, so `7 2` is safe: it proves the
  detection (`host: Retry ... -> the newest autosave`), the approx offer and the guest's in-world load; the host stays
  where it is. Where a real retry puts the host (versus the newest autosave) is still unmeasured.
* **A New Game's first save.** The intro stops at HDR Settings (START / A did not pass it in 150 s, run
  20261119-074037) and its opening runs far longer than a run. Proven instead: the New Game detection, the "you join at
  their first save" state on both screens, the baseline, and the poll -> offer -> guest load path with an existing slot
  standing in (`9 4 18`).

**Still not verified:** the virtual-pad fallback (every byte check passed, so it never ran); a host read colliding with
the game's own rename of the same slot (the game's MoveFileExW fails while our handle is open, proven on scratch files;
the reader now skips a slot with a `checkpoint.dattemp` and holds its handle only for one read, but a save that starts
during those milliseconds can still lose its rename; no collision seen); the title-ready wait is time, not a signal
from the game (a guest whose title takes longer than ~30 s to come up, or a notice left open, is not covered); an
emergency stop and the several-accounts refusal (unit logic only, no hidden run makes the tick fault or seeds two
accounts); the guest's online features (D1-8); the host's real Retry spawn versus the newest autosave; a real New
Game's first autosave. Leave closes the game with WM_CLOSE. (Before 2026-11-20, behind the full-screen panel, A on
CONTINUE, a guest back at its title with manualsave5/6, was refused by the gate and the title stayed black behind the
panel, run 20261119-072416; the native title now hides CONTINUE and ignores A.) The waiting screen's host line ("The host is in the game") is the last state
heard while the link is down.
