# Shared structures (world sync)

Structures either player builds with the PCC appear in both worlds, with the game's own build-up and printer on both
screens, and stay in the host's save. The host's world decides everything; the guest's world is a disposable mirror (the
host-held save, docs/coop-session.md). Code: `native/mod/world_share.*` (game side), `native/mod/world_wire.*` (the
messages, pure), `native/mod/world_rules.hpp` (the rules, pure), `native/mod/online_gate.*` (never uploading). Tests:
`tests/world_wire_tests.cpp` (ctest `st_world_wire_tests`), `tests/dev_rules_tests.cpp`, `native/testenv/testenv_tests.cpp`.

Status: **W1**: PCC structures, both directions, the build-up and printer, dismantles both ways, persistence. **W2**
(section "Contributions"): both players pay into the same structure (upgrades, the first build of one that needs
materials, repairs) from their own inventories toward one shared total; the level-up plays in both worlds; durability is
shared. **W3** (section "Ladders and climbing ropes"): ladders and climbing anchors / ropes placed by either player
appear in both worlds, either player can use the partner's, a collapse / recovery (the game's pick-up) removes them from
both worlds, they stay in the host's save; world sync is **on by default** with `host_save=1`. **W4** (section
"Ziplines (W4)"): ziplines either player builds (the real PCC UI too) appear in both worlds, link to the same ziplines in
both, take both players' upgrade materials, are dismantled from both and stay in the host's save; the shared ladders' and
ropes' durability follows the host's. Not shared (section "Not shared yet (after W4)"): signs, dropped cargo, locker /
postbox contents, auto-paver roads.

## Turning it on

| session.ini key | Values | Meaning |
|---|---|---|
| `world_sync` | `0` / `1` | The feature. Needs `host_save=1` and the role host or guest; both games must have it on (HELLO). **W3: on when the key is absent and `host_save=1`** (`[config] world_sync=1 (default: on with host_save=1)`); `world_sync=0` turns it off. The launcher writes it for Host, Join and the Local test from the Debug option **Share structures, ladders and ropes** (CO-OP SAVE, default on; `world_sync=0` when off). |
| `world_ladders` | `1` (default) / `0` | W3: ladders and climbing ropes. Both games must have it (HELLO parts bit2); off on either side, or a failed `ladder` byte check, = each world keeps its own (`[world] ladders and ropes OFF: ...`). |
| `world_ziplines` | `1` (default) / `0` | W4: ziplines. Both games must have it (HELLO parts bit3); off on either side, or a failed `zipline` byte check, = each world keeps its own (`[world] ziplines OFF: ...`). |
| `world_contrib` | `1` (default) / `0` | W2: contributions, upgrades, repair and durability. Both games must have it (HELLO parts bit1); off on either side, or a failed `contrib` byte check, = each world keeps its own numbers (`[world] contributions OFF: ...`). |
| `world_anim` | `645` (default) / `loaded` / `off` | How a mirrored structure is created: `645` = the game's own new-build path (record +0x645: the build-up and, with a place, the printer); `loaded` = as a structure loaded from a save (+0x644: no build-up, no mission event, no stats); `off` = no animation. `bits` is accepted and falls back to `645` in this build. |
| `world_test_fail` | a byte-site name part | Hidden runs only: that byte site counts as failed (the off path's test, `two-games-world-off`). |

Log line: `[config] world_sync=1 contrib=1 ladders=1 ziplines=1 anim=645`. When it is off: `[world] world sync OFF: <reason>`
(`world_sync=0`, `world_sync=0 (not in session.ini and no host-held save session)`, `needs the host-held save session
(host_save=1)`, `this game build differs (<site>)`, `the online gate is not in (...)`).

## What happens

**Identity.** A structure is the host's construction point (CP). Its key is the host's CP id (`hid`, Player ids
10000+n); the guard is its type and position (0.5 m). The guest loaded the host's save, so every saved structure has the
same id in both worlds.

**Host builds** (its PCC, or `dev_build`). The game's own commit runs untouched; the `EnqueueInfo` detour copies the
record and the `SetLastBuilt` detour the printer's place. The census (every 500 ms) sees the new CP and sends `ADD`
{record, printer place, anim}. The guest mirrors it with the same id (`exact`; a `mapped` id if hers is taken) through the
game's own creation queue with +0x645 (the build-up) and `SetLastBuilt` (the printer), offline bit set. Log:
`[world] host: new structure hid H type T (postbox) origin host anim 1 printer 1 offline 1 -> ADD`, then
`[world] guest: mirror hid H -> gid H (exact) type T (postbox) anim 1 printer 1 (the host's build)` and 3 s later
`[world] guest: mirror hid H ok: gid H state 2 level 0 offline 1 server id empty`.

**Guest builds.** Her commit runs (the PCC item really leaves her inventory) but the `EnqueueInfo` detour HOLDS the record
(it never enters her queue) and `SetLastBuilt` is deferred. `GBUILD` {request, record, printer place, her body} goes to the
host, which checks it (`world_rules::ValidateGuestBuild`: a valid record, a synced Player type, within 60 m of her body; a
dev build only if the host's own saves are sandboxed) and creates it with its own `NewId`, offline bit set, the build-up
and the printer at her PCC. The census then sends `ADD` {origin guest, her request}; she mirrors it with the host's id and
plays her printer and build-up. Log: `[world] guest: local build type T (generator) held, request #r sent`,
`[world] host: the guest's build #r: created hid H ... offline 1 anim 1 printer 1`, `[world] guest: build #r confirmed as
hid H`. If the host's game does not share structures (no HELLO with `on`), her build stays hers, as without the feature
(`[world] guest: local build id N ... NOT shared`).

**Join and every load.** After each guest load: `HELLO`, `SNAP_REQ` (her world counter = the host's load number), `SUM`
(64 bucket hashes over the host's shared structures: FNV-1a of id, type and position in cm, bucket = id & 63). Equal
buckets bind at once; differing ones go `BUCKET_REQ` -> `DIGEST` (the host's entries of those buckets) -> bind / `NEED`
(the host answers `ADD` without animation) / remove the guest's extras. Log: `[world] guest: snapshot #n complete: bound
B, created C, removed R, replaced P`. Every 2 s the host sends `CHECK` (one hash over the 64); two misses in a row ->
`SNAP_REQ` (heal), at most one per 10 s. A bound structure that vanishes from the guest's world without a `REM` -> `LOST`
(the host re-sends `ADD`). The guest raises her own per-category Player limits by 256 in memory on her first join, so
only the host's evictions remove anything.

**Removals.** The host's census classifies each disappearance (`world_rules::ClassifyRemoval`): `manual` (RemoveManually:
+0xAD bit 7 or the detour saw it), `evicted` (a structure of that category appeared in the same tick), `decay`
(durability 0), `area`; `REM` goes out before the tick's `ADD`s. The guest applies `REM manual` with the game's dismantle
(`RemoveManually`, its animation), the others with the game's own removal bits (+0xAA |= 0x40, +0xAB |= 1, under the
manager's lock, as `RemoveConstructionsInArea`). A guest dismantle of a bound structure runs natively and sends `REMREQ`;
the host removes it with `RemoveManually` (guard checked; else it re-sends `ADD`).

**Persistence.** The host's copy of every structure is an ordinary own (Player) CP with the game's offline bit; the
game's own save writes it (the save section is a fixed 3140 records, nothing grows), and the next load creates it from the
save like any other. The guest's world is never saved for real (her sandbox); next session she loads the host's newest
save and binds by id. Proven: `two-games-world-persist` (below).

**Epochs.** Every message carries the host's world counter; a stale one is dropped. A new link session sends `HELLO`
again; the guest re-sends a held build (`retry` flag; the host answers a known request id with the same `ADD` and binds a
same-type-and-place build instead of creating it twice). A host reload makes the guest reload (the host-held save), and
both start over.

## Never uploading (`online_gate.cpp`)

The game uploads a player's own new structure (`RegisterNew` -> `createObject`), deletions, materials
(`setConstructionMaterials`) and telemetry. Three locks keep the mod's structures off the game server:

1. Every structure the mod creates carries the game's own offline bit: record +0x62E -> cp+0xAB bit 7; both
   `createObject` builders (0x142CAC820 / 0x142CADA10) return at once on it.
2. The gate's detours drop the online service's calls: `RegisterNew` 0x143485040 (vt+0x28), the removal drain
   0x143482F30 (vt+0x38, one structure per call), telemetry 0x143484AC0 (vt+0xB8; dropped by calling it with no object,
   its own early-out that releases the event) and the materials upload 0x142CB5BC0. Host (real use): for offline CPs, the
   mod's CPs and inside the mod's own calls; the host's own normal structures stay vanilla. Guest: everything (her world
   is a copy of the host's save; her copies of the host's uploaded structures carry the host's server ids), **for the whole
   session**: the gate is locked closed when it goes in for a world-sync guest, and a stop of world sync (a fault, the co-op
   session ending, a failed world byte check) never reopens it; only the host falls back to vanilla after a stop. The mod's
   removals on the host are remembered by pool slot AND structure id (cp+0x30): a slot the game reuses for the host's own
   later build is vanilla again. **Every
   hidden run, both games, everything** (the gate is installed in automation even with world sync off: a hidden run's
   host is signed in to the game server with the player's account).
3. A logger on the OTGC request constructor 0x141B91D30 logs each request name the game builds (reads once:
   `[gate] OTGC request (read): urn:otgc_find_qpid_objects (first of its kind)`; writes every time:
   `[gate] OTGC WRITE request: <name>`, `world_rules::OtgcWriteName`: create / update / delete / set / send / devote /
   add / remove / post / put / upload / register / like). The hidden-test supervisor counts the WRITE lines of both games
   as `game_server_writes` and fails the run on any; `online_blocked` is the gate's dropped total.

4. (W2) The likes manager's sendLike sender 0x142E14820 (the only builder of `urn:otgc_send_like`: it calls the SendLike
   descriptor 0x1432B4590 and the OTGC ctor) returns at once in every hidden run (both games) and on a world-sync guest.
   Likes queue up from AddOwn on others' structures and from driving on highways (`HighwaysSendLikeProcessInterval`): run
   `20261121-164550-two-games-ride-truck` (world sync off) FAILED on two `urn:otgc_send_like` requests the guest's game
   built while riding the truck on the highway; with the gate: `[gate] dropped the likes manager's sendLike (hidden run)`,
   0 writes (run `20261121-165053`). Real use, host or solo: vanilla.

Counters: `[gate] counts: dropped register n, drain n, telemetry n, upload n, likes n (total n); passed ...; OTGC
requests n (write n)` (action `world_sync 3`).

5. (round 5) The guest's repair of a host structure changes only its durability on the host (vt+0x140), never the host's
   repair bucket +0x260: the materials upload sends +0x210 / +0x260 as absolute totals the next time the host itself pays
   at that terminal, and the guest's materials must not reach the server under the host's account. Her +0x260 follows the
   host's by STATE. The hidden-test supervisor also requires `[gate] online gate ON` in every modded game's log (summary
   `online_gate`, `online_gate_ok`; a game whose mod stopped at start-up on an unsupported ds.exe counts as inactive), and
   the world-changing world tools refuse in a hidden run without the gate.

### Round 5: robustness of the session

* **Restarted partner.** Every game numbers its SWS1 messages from 1; the duplicate window is now per sender process: the
  HELLO carries the sender's random nonce (`Hello.sender`, 4 bytes), a new one starts a fresh window (`[world] the partner's
  game is a new process ...`). The same process keeps its window across link sessions. A HELLO is never dropped as a repeat.
* **Reconnect in the same world.** A new link session on the guest starts a new snapshot (`SNAP_REQ` reason 2, reconnect):
  the host forgot her (`g_guest_in_world`) and messages of the old session may be lost both ways. Her REMREQs (dismantle,
  pick-up) are sent again once per new link session before the SNAP_REQ, and kept until a snapshot asked after them
  completes. Lost host messages (ADD, REM, STATE) are healed by that snapshot and the STATE at her join.
* **Stale build requests.** The host's GBUILD bindings are per world (cleared on a reload: the Player id counter goes back
  with an older save) and a binding is used only while that id is still the same type at the same place.
* **Emergency stop.** It also stops sharing on the guest (contributions, ladders, ziplines): her terminal and level-ups are
  vanilla again, nothing is queued for a tick that no longer runs.
* **Only the terminal's contributions.** The guest forwards AddOwn only from the terminal commit (return 0x142CF996D) and
  AddRepair only from the repair commit (0x142D32B97), or from `dev_build_mats`. AddOwn's other caller, the story's
  `SetConstructionLevel` export (0x142CDB16E), runs in the host's world from its own copy of the script.
* **Her pick-up needs the circumstances of one** (`wrules::PickupPlausible`): the census saw a world (an empty live list is
  never a pick-up), at most two bound ladders / ropes went in that census tick, and her body was within 20 m of it.
  Anything else is LOST: the host keeps its REAL ladder. Positive evidence (a detour on the pick-up action itself) is not
  built yet.
* **A mirror her world keeps removing** is LOST at most 3 times per world, then given up (one line); the join check's heal
  backs off (10 s doubling to 320 s) while heals change nothing.
* **Both players pick up the same ladder at once:** one item each. The host's world and save are right (picked up once
  there); her extra item lives only in her sandboxed copy of the host's save. Both games log it.
* **Dev removals on the guest** (`dev_build_remove`) go with the dev flag; the host applies them only when its own saves
  are sandboxed (`dev::DecideNow`), else it refuses and sends the structure again. The bits path is never a pick-up.
* Proof (batch `round5-fixes`, 21 of 21 passed, both games `online gate: on`, 0 writes, saves unchanged): the W1..W4
  scenarios (the real terminal path `20261121-220354-two-games-world-contrib-ui` still forwards her payment; the guest's dev
  removal goes as `REMREQ (dev: the host decides)` in `20261121-220617-two-games-world-build`; real and dev pick-ups still
  pass the new checks in `20261121-221301-two-games-world-ladder-ui` / `20261121-220834-two-games-world-ladders`).

## The wire: link reliable channel 1 "world", `SWS1`

Header 16 bytes: `"SWS1" | u8 type | u8 version 1 | u16 flags (1 anim, 2 dev, 4 retry) | u32 host world | u32 seq`.
Strict decoders (exact sizes, known types, finite numbers, positions within 20 km, levels 0..5). A per-sender `seq` and a
256-entry window drop repeats across link sessions. 1024 bytes per message, an outbox of 512 drained while the channel
holds fewer than 24.

| # | Message | Dir | Body |
|---|---|---|---|
| 1 | HELLO | both | protocol, on, parts (bit0 build, bit1 contributions, bit2 ladders and ropes, bit3 ziplines (W4)), anim mode, ready, the sender's world |
| 2 | SNAP_REQ | G->H | the guest's world counter, reason (0 load, 1 heal, 2 reconnect) |
| 3 | SUM | H->G | count + 64 bucket hashes |
| 4 | BUCKET_REQ | G->H | 64-bit bucket mask |
| 5 | DIGEST | H->G | serial, part / parts, mask, up to 24 entries {hid, type, state, level, flags, pos f64[3]} |
| 6 | NEED | G->H | up to 64 hids |
| 7 | ADD | H->G | hid, origin (save / host / guest), request id, printer place, the Record |
| 8 | GBUILD | G->H | request id (session nonce, n), printer place, the guest's body, the Record |
| 9 | REJ | H->G | request id, reason (invalid, type, disabled, far, not_ready, host_real) |
| 10 | REM | H->G | hid, type + position (guard), reason (manual, evicted, decay, area, pickup, guest) |
| 11 | REMREQ | G->H | as REM (W3: reason pickup = she collapsed / recovered a ladder or rope: the host removes its copy with the removal bits) |
| 12 | MATS | G->H | W2: request id, hid, guard (type, position), kind (0 materials, 1 repair), amounts int[6] (0..100000, at least one) |
| 13 | STATE | H->G | W2: hid, guard, level, the config's last level, level-up (0 none, 1 queued, 2 running), why (change, join, applied, retry), own int[6], invested int[6], repair int[6], durability, the request it answers, excess int[6] |
| 14 | DUR | H->G | W2: 1..100 x {hid, durability}; W4: the shared ladders and ropes too |
| 15 | CHECK | H->G | count, one hash over the 64 buckets |
| 16 | LOST | G->H | hid |

**The Record** (496 bytes) is exactly the construction record's plain-data fields the game's save writer (WriteRecord
0x142CA7970) persists, plus the placement check's snapped transform: info +0x000..+0x003, +0x008..+0x043 (transform),
+0x04C (level), +0x070..+0x0A7 (state, durability, own materials ...), +0x2D8..+0x2EF, +0x520..+0x5E3, +0x5E8..+0x603,
+0x608..+0x61F (timestamps), +0x620..+0x643 (flag bytes), +0x680..+0x6BF. Never: the id, the owner handle, the UUID, the
slot, the server id string, the contributor lists, the runtime flags +0x644.., the +0x650 reference, the stored-cargo list
+0x670 (re-created as baggage on creation: copying it would duplicate cargo). The receiver sets the id, its own local user
as owner, UUID 0 (a non-zero equal UUID would make the queue move an existing CP), +0x62E = 1 (offline), +0x644 / +0x645
by `world_anim`. A host record comes from the game's own ExportInfo (cp vt+0x48) into a ctor-built temporary.

## What the game needs for a build-up (found in W1)

* **+0x645** "new local build": InitFromInfo sets cp+0xAB bits 0x08 / 0x40 and cp+0xAE 0x20 (not for ladders / ropes).
* **+0x623 "under construction"** (cp+0xA8 bit 7): the CP update 0x142CB8790 runs the level 0 -> 1 build timer only with it
  (`(char)cp[0xA8] < 0`), and the build machine stays attached while it is set (0x142C809F0). InitInfo 0x142CD4EF0 does
  not set it; the PCC preview's record carries it. Without it the CP stayed state 0, level 0 (run
  `20261121-151026-world-build-solo`).
* **The printer's place** is the build preview's transform (the commit passes player+0x308 -> +0xA8, the hologram at the
  structure's spot), not the player's hand: printing starts (0x142CB4EE0) once the printer stands within 0.1 m of its
  target.

## Contributions (W2): one shared total, the level-up in both worlds

Each player pays at a structure's own terminal (Structure Menu: Upgrade / Repair) from their own inventory, as in the
game; the host's world keeps the one total and decides the level-up; the guest's numbers follow.

**What the game does** (decompiled; `world_rules.hpp` "contributions"):

* `AddOwn` 0x142C41540 (the terminal commit 0x142CF98E0 calls it after clamping the amount): cp+0x210 += m; telemetry
  (`TelemetryDSOnlineObjectAppendMaterials` through the online service vt+0xB8), stats, likes for others' structures.
  It **never writes the level** (+0x84): it only computes the level the new total reaches, for the stats.
* `AddOthers` 0x142C321A0 (cp, stamp, m, force, road): cp+0x248 += m while level < max; nothing online, no stats, no
  likes. The game's own bucket for online players' contributions.
* `Invested` 0x142C58A50 = own (+0x210) + the contributor list (+0x228, 0x38-byte entries, int[6] at +0x10) + others
  (+0x248). `NeedToLevel` 0x142C5D9D0 (cp, L) = what levels 1..L need in all (cumulative). Materials are never consumed:
  two worlds with the same totals reach the same level.
* `Status` 0x142C59770: 0 enough for the next level, 1 missing, 2 last level, 3 no levels, 4 a level-up runs (cp+0xA8
  bit 7). Its two outputs are both relative to the current level (minus NeedToLevel(level)).
* `QueueLevelUp` 0x142C69170 (mgr, cp): the id into the manager's level-up list +0x56A2A8 (under the CS). The manager
  update (0x142CBAD0A..0x142CBAD65) calls, for each id, cp vt+0xC0 `StartLevelUp`(cp, the level the materials reach,
  0x142C35CC0): cp+0xA8 |= 0x80, the level's timer and durability, the mission event, the level-up message to the entity
  (the countdown on the structure), and for level 0 -> 1 the build machine (printer). The level is written when the
  timer ends (~17 s for a postbox 1 -> 2, run `20261121-160128-world-contrib-solo`).
* Repair: the repair commit 0x142D32AA0 calls cp vt+0x140 (base 0x142CA2D30: durability += sum(m[i] x config +0x210[i]),
  capped at the level's) and `AddRepair` 0x142C41CB0 (cp+0x260 += m; telemetry).

Seen in the 100% save: postbox level 1 of 3, level 2 needs 200 metals in all, level 3 800; watchtower level 2 needs
32 crystals + 400 metals, level 3 182 + 1200; repair: 20 metals on a postbox = +4800 s, 20 crystals on a watchtower =
+4800 s.

**Which structures**: `wrules::Contributable` = the synced Player structures and the Stage (story) structures of the
same categories (same ids in both worlds: the same save). Not: roads (the auto-paver, category 14: the highway system
and its own uploads, W4), Net structures (each world keeps its own online ones), ladders, ropes, ziplines, cargo pods.

**The guest pays**: her terminal commit runs natively (her cargo / crystals leave her inventory), `AddOwn` runs in her
world (the gate drops its telemetry; her terminal shows the new numbers at once) and our detour queues `MATS` {request,
hid, guard, amounts}; her own `QueueLevelUp` on a shared structure is swallowed (`[world] guest: our level-up request on
id N (postbox): the host's call (swallowed ...)`). The host (game thread) checks the guard, clamps the amounts to what
the structure still needs for its LAST level (`ClampContribution`: surplus counts toward the next level, as the game's
terminal would; beyond the last level it is the excess, reported back), applies them with `AddOthers` (under the mod's
gate scope), and if `Status` is 0 and no level-up waits or runs, `QueueLevelUp`: `[world] host: the guest's
contribution #r on hid H (postbox): applied 0/0/100/0/0/0, excess 0, level 1, status 0: level-up queued`. It answers
`STATE` (why applied, her request, the excess). After a level-up ends the host re-checks: materials beyond the next level
queue the one after. A request seen before (a retry after a new link session) is answered, never applied twice
(`ReqLog`). A dev contribution (`MATS` with the dev flag) is applied only if the host's own saves are sandboxed.

**The host pays**: its terminal is vanilla (AddOwn, its own QueueLevelUp; in real use the game uploads its materials).
The census (every 500 ms) sees the change and sends `STATE` (why change): `[world] host: hid H (watchtower) level 1 (a
level-up runs) -> STATE`.

**The guest follows** (`STATE`, absolute): own (+0x210) = the host's own, others (+0x248) = invested - own - her
contributor list (so her `Invested` equals the host's), repair (+0x260), durability (+0xD8), under the manager's CS.
If the host is ahead or its level-up is queued / runs, hers is not, and her materials reach a higher level, she queues
the game's own level-up (`GuestShouldQueueLevelUp`): `[world] guest: hid H (postbox): the host's level 1 (its level-up
queued); ours 1 -> level-up queued (the materials reach 2)`. The countdown and the level-up play on both screens
(screenshots below). While her own MATS waits for its answer, other STATEs for that structure do not overwrite her
numbers; an answer that never comes is given up after 20 s (the host's numbers win). A STATE for a structure not (yet)
in her world waits up to 15 s.

**Both at once**: the host's own terminal and the guest's MATS on the same last units: both totals count (the surplus
toward the next level), and the host drops a second level-up request while one waits or runs (`H_queue` on the host:
`[world] host: a second level-up request on hid H ... dropped (it plays once)`); exactly one level-up in each world
(run `two-games-world-contrib-persist`).

**Durability** (`DUR`, every 5 s): the host's contributable structures whose durability moved more than 2 % since the
last sent (or crossed 0, or is below 10 % of the level's), at most 200 per round; all of them again at the guest's join.
`[world] guest: DUR n: n applied, m moved more than 2 % (the most: hid H 539949 -> 595 s)`. A collapse is the host's
removal (`REM decay`, W1).

**Join**: the host remembers every structure whose numbers changed since its world loaded and sends their `STATE` (why
join) after the guest's `SNAP_REQ` (`[world] host: contributions: STATE for n structure(s) changed since this world
loaded; durabilities follow (27 tracked)`). The structures she gets by `ADD` carry their numbers in the Record (+0x90
own, +0x528 others, +0x558 repair).

**Persistence**: the host's numbers are its real structure's (own +0x210 and others +0x248 are in the saved record); the
next session the guest loads that save. Proven: `two-games-world-contrib-persist` (level 2, invested 0/0/400 in both
worlds after the host's save and an in-world load; a contribution after the load works).

**Not shared**: if the host does not take contributions (its HELLO without parts bit1, or not joined yet), the guest's
payment stays in her world (`[world] guest: a contribution on id N (postbox) is NOT shared ...`), as without the
feature; her level-up request is not swallowed then.

## Ladders and climbing ropes (W3)

Ladders (`DSLadder`, Player type 25, category 10) and climbing anchors with their ropes (`DSFieldRopeConstructionPoint`,
type 28, category 11) are construction points like the PCC structures, so they ride the same pipeline: the census tracks
them (`wrules::IsSynced(type, ladders)`), the host's new ones go out as `ADD`, the guest's own placement is HELD and sent
as `GBUILD`, removals as `REM` / `REMREQ`, the join check binds the saved ones by id (the 100% save's 10 Player ladders and
4 ropes are shared from the first join: `bound 31` instead of 17). No printer and no online registration (the game skips
both for categories 10 / 11); no levels to pay into (not `Contributable`).

**What the game does** (decompiled):

* **Placing.** The player's commit 0x1428D8070 handles both: a ladder is the PCC branch (held item 0x5C: the preview's
  record, NewId, owner, +0x645, `EnqueueInfo` at 0x1428D8767; no SetLastBuilt, no item consumed by 0x14293A340); a
  climbing anchor (held item class '3'..'5') builds its own record (type forced to 0x1C = 28, level 0..2 from the item,
  durability = config +0xA0[level] x the item's, +0x623 / +0x645) and enqueues it at **0x1428D82A9**. Both calls go
  through our `EnqueueInfo` detour: the guest's are held exactly like her PCC builds.
* **The record's subclass fields** (all inside the Record's +0x520..+0x5E3 range, so W1's wire carries them unchanged):
  ladder InitFromInfo 0x142D2DD00 / ExportInfo 0x142D3C870: +0x578 i32 mode, +0x57C / +0x580 f32 lengths (<= 0: 9.65 m),
  +0x584 / +0x588 i32 flags (== 1); rope InitFromInfo 0x142D24530 / ExportInfo 0x142D24470: +0x578 f32 length (clamped
  1..30), +0x57C f32[3] direction, +0x588 f32 longest (<= 0: 30). The rope's ExportInfo reads its simulator component
  (realized ropes only; unrealized: length 1, direction (0, 1, 0), 30). `world_wire::SubclassValid` checks them
  (finite, lengths <= 100 m, the direction within +-1000).
* **The placement check** 0x142C38200 (mgr, info, reasons[0x23] x 16 B, result, out, ignore, u8, u8), called by the build
  preview (0x1428B200D) and the re-checks: per category (ladder 0x142C3C070, rope 0x142C3F120) it snaps the transform onto
  the ground / an edge and fills the geometry. A ladder on a spot it refuses is removed a moment later by the ladder's own
  re-check (vt+0x60 0x142D2E340: `0x142C421E0(mgr, 10, pos, 1)` false -> the removal bits): run `20261121-173040`, a dev
  ladder on the flat road gone after 3 s. `dev_ladder` / `dev_rope` therefore run the game's check (the first of a few
  spots around Sam it accepts).
* **Picking up** ("Hold START: Collapse Ladder" / "Recover Rope"; the player actions `LadderToBaggage` 0x38 /
  `DestroyLadder` 0xAA / `DestroyRope` 0xAB) gives the item back to that player, and the construction point leaves the
  live list without the removal bits, `RemoveManually` or `Remove` (watch, run `20261121-174141`: cp+0xB0 bit 0x40 for a
  tick, cp+0xA9 bit 0x40 cleared, then gone; a rope's recovery passes through state 3 with +0xAA bit 0x40 clear, run
  `20261121-184858`).
* **Re-attaching.** Each world re-attaches a realized ladder to the ground (0x142D2E5B0, ray casts), which moved one by
  1-5 cm between the two worlds (runs `20261121-175940`, `180840`). So a ladder's / rope's join-check hash leaves the
  position out (id and type only; the join check healed every 10 s before) and their guard tolerance is 0.3 m (the
  binding still compares places).

**How it is shared.**

* **Placing**: as the PCC structures (sections above): the host's placement -> census -> `ADD` -> the guest mirrors it with
  the host's id; the guest's placement is held -> `GBUILD` -> the host creates it (offline 1, its own `NewId`) -> `ADD`
  with her request -> she mirrors it. Logs as W1 with `type 25 (ladder)` / `type 28 (climbing rope)`, `printer 0`.
* **Picking up**: the host's pick-up: its census sees the ladder gone with durability left: `ClassifyRemoval` -> `pickup`
  (`[world] host: hid H (ladder) removed (pickup), gone -> REM`); the guest removes her copy with the removal bits (no item
  for her). The guest's pick-up: her census sees a bound ladder / rope vanish without a `REM`, with durability left
  (`wrules::GuestVanished` -> pickup, from the last census's look at it) -> `REMREQ pickup`
  (`[world] guest: we picked up hid H (ladder, durability N s left) -> REMREQ pickup`); the host removes its copy with the
  game's removal bits under the mod's gate scope, never RemoveManually or a pick-up, so **the host gets no item**
  (`[world] host: the guest picked up hid H (ladder): removed here too (removal bits, no item)`); no `REM` back.
* **The game's own removals are not pick-ups**: detours on the ladder's re-check (vt+0x60 0x142D2E340), its ground
  re-attach (0x142D2E5B0, also called by the rope's) and the rope's re-check (vt+0x60 0x142D24610) note a removal they
  flag, and `Remove` 0x142CA3E00 notes its caller for ladders / ropes (`kAutoRemoveReturns`); such a vanish on the guest is
  `LOST` (the host sends it again), as is one the census still saw with the game's removal bits (+0xAA bit 0x40:
  `RemoveConstructionsInArea`, a collapse) or with no durability left (her copy decayed first).
* **Climbing**: each player climbs the real ladder / rope in their own world (the game's own "Grab Rope"); the partner's
  body on it is the existing pose sync. Run `20261121-174141` (R2 / R3): the guest on the host's rope ("Release Rope",
  28/30 m).
* **Own evictions off**: the guest's per-category limits for ladders / ropes (128 each in the 100% save) are raised by 256
  in memory with the others.
* **Not shared**: if the partner's game does not share them (`world_ladders=0`, a failed `ladder` byte check, an older
  build: HELLO without parts bit2) each world keeps its own; a guest placement then stays hers
  (`[world] guest: local build id N type 25 (ladder) NOT shared: the host's game does not share ladders and ropes`).

**The real UI with the hidden env's pad** (calibrated in `world-ladder-ui-probe`): D-pad right held = "Select Tools" (a
radial: the right stick about 5 o'clock, `rx=18795 ry=-26840`, picks "Custom Chiral Ladder"; straight down the climbing
anchor), LT held = "Adjust Position" (the hologram: red where it cannot go, e.g. standing on flat ground), RT = "Place
Ladder" / "Place Anchor". Near a placed one: "Hold START: Collapse Ladder" / "Recover Rope", X = "Grab Rope" / "Release
Rope". A START tap opens the pause menu.

## Debug and automation actions

See docs/debug-actions.md "World sync": `world_dump`, `world_sync <op>`, `dev_build`, `dev_build_remove`,
`dev_build_decay`, `dev_build_mats` (W2), `dev_ladder`, `dev_rope`, `world_sync 12 / 14 / 15` (W3), `world_sync 16` (W4: zipline links; `dev_build 22` builds a zipline). The dev tools (`Tool::structure`, dev_rules.hpp) run only on sandboxed saves; on a co-op guest they
ask the host (a dev `GBUILD`, created only if the host's own saves are sandboxed).

## Byte checks

At start-up (`world_share.cpp kSites`, `online_gate.cpp kSites`), before any detour. Groups: core (the manager and the
record) and build (ids, owner, printer) off -> the whole feature off; remove off -> removals are not mirrored; contrib
off -> contributions are off (`[world] contributions OFF: this game build differs (contrib byte check)`); ladder off ->
ladders and ropes are off (`[world] ladders and ropes OFF: this game build differs (ladder byte check)`); zipline off ->
ziplines are off (`[world] ziplines OFF: this game build differs (zipline byte check)`). The type
table (`world_rules.hpp kTypes`) is compared with the game's own TypeToCategory / TypeToNetType for all 46 types.

| Address | Bytes (prefix; `??` = rip-relative) | What |
|---|---|---|
| 0x142CD6C1D | `48 83 3D ?? ?? ?? ?? 00` (rel -> 0x147BF4960) | the construction manager global |
| 0x142C47AF0 | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D 99 B8 A5 56 00 48 8B F1` | EnqueueInfo (detour) |
| 0x142C85CE0 | `4C 8B DC 55 41 55 49 8D AB B8 F7 FF FF 48 81 EC 38 09 00 00` | ProcessCreationQueue |
| 0x142C86400 | `80 BD 05 07 00 00 00` | its +0x645 test |
| 0x142C86432 | `FF 50 28 C7 86 18 05 00 00 00 00 F0 42` | its RegisterNew call |
| 0x142C57350 | `48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 28 8B DA` | FindById |
| 0x14277FDE0 | `48 89 5C 24 08 ... 41 56 48 83 EC 50 C6 01 00 B8 FF FF 00 00` | record ctor |
| 0x1428ACA00 | `48 89 5C 24 08 ... 57 48 83 EC 20 48 8B E9 48 81 C1 70 06 00 00` | record dtor |
| 0x142C575D0 / 0x142C32760 | `48 89 6C 24 20 56 57 41 57 48 83 EC 20 8B 41 30` / `48 89 5C 24 18 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 83 EC 30 8B 42 48` | ExportInfo / InitFromInfo (base vtable 0x143DD9408 +0x48 / +0x50) |
| 0x142C32AB4 | `0F B6 87 2E 06 00 00 F6 D8` | InitFromInfo reads +0x62E (offline) |
| 0x142CCF1D0 / 0x142CCF0F0 | `0F B6 C1 83 F8 2D 77 21 48 8D 15 ?? ?? ?? ?? 0F B6 84 02 08 F2 CC 02` / `... 77 4E ... 94 F1 CC 02` | TypeToNetType / TypeToCategory |
| 0x142C5BD80 | `40 53 44 0F B6 DA 48 8B D9 84 D2 74 36 80 FA 02` | NewId (Player: ++[mgr+0x572D34] + 10000) |
| 0x1434ABDB0 / 0x1434AC430 / 0x1434ABFF0 | `40 53 48 83 EC 20 48 8B D9 89 11 48 8B 0D ?? ?? ?? ??` / `48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 21 8B 02 39 03` / `48 8B D1 48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 85` | owner handle / copy / free |
| 0x142CB5280 | `48 89 5C 24 08 ... 41 56 48 83 EC 20 48 8D 99 B8 A5 56 00 48 8B F9` | SetLastBuilt (detour; the printer) |
| 0x1428D8756 / 0x1428D8767 / 0x1428D87F2 | `C6 83 F5 07 00 00 01` / `E8` -> EnqueueInfo / `E8` -> SetLastBuilt | the PCC commit |
| 0x142CD4EF0 | `48 89 5C 24 10 56 57 41 56 48 81 EC D0 00 00 00` | InitInfo (dev builds) |
| 0x142CA4060 | `48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 80 89 AA 00 00 00 40` | RemoveManually (detour; group remove) |
| 0x143485040 | `48 89 74 24 18 57 48 83 EC 20 48 8B 02 48 8B F1 48 8B CA 48 8B FA FF 10 48 8D 4E 38` | gate: RegisterNew |
| 0x143482F30 | `40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 48 89 7C 24 48` | gate: removal drain |
| 0x143484AC0 | `40 53 55 56 57 41 56 48 83 EC 40 48 8B AC 24 90 00 00 00` | gate: telemetry |
| 0x142CB5BC0 / 0x142CB5C10 | `40 55 57 41 55 48 8D 6C 24 C0 48 81 EC 40 01 00 00` / `48 81 C2 B8 00 00 00` | gate: materials upload, its server-id read |
| 0x141B91D30 | `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F1 48 89 01` | gate: OTGC request ctor (logger) |
| 0x142CAC843 / 0x142CADA35 | `80 BA AB 00 00 00 00` | createObject's offline tests |
| 0x143EDECC8 | slots +0x28 / +0x38 / +0xB8 = the three gate targets | the online service's vtable |
| 0x142E14820 | `4C 8B DC 55 41 54 41 57 49 8D AB 48 FF FF FF 48 81 EC A0 01 00 00 48 8B` | gate (W2): the likes manager's sendLike sender (detour) |
| 0x142E14BC9 / 0x142E14C04 | `E8` -> 0x1432B4590 (the SendLike descriptor) / `E8` -> 0x141B91D30 (the OTGC ctor) | gate (W2): the sender builds the sendLike request |
| 0x142C41540 | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 70 FF FF FF 48 81 EC 90 01 00 00` | contrib: AddOwn (detour) |
| 0x142C321A0 | `40 53 57 41 57 48 83 EC 40 4C 8B BC 24 80 00 00 00 49 8B F8 48 8B D9 45 84 C9 75 0D 48 39 91 58` | contrib: AddOthers |
| 0x142C41CB0 | `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00` | contrib: AddRepair (detour) |
| 0x142C59770 | `40 55 53 56 57 41 56 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F8 33 C0 4C 8B` | contrib: Status |
| 0x142C69170 | `48 89 74 24 18 57 48 83 EC 20 48 8B 81 60 A2 56 00 48 8B FA 4C 63 81 58 A2 56 00 48 8B F1` | contrib: QueueLevelUp (detour) |
| 0x142C5D9D0 | `40 55 56 41 56 41 57 48 83 EC 48 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 30 33 C0 4D 8B F0` | contrib: NeedToLevel |
| 0x142C58A50 | `48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 33 C0 48 8B F1 48 89 02 48 89 42 08 48 89 42 10` | contrib: Invested |
| 0x142C35CC0 | `48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4` | contrib: the level the materials reach |
| 0x142CBAD0A / 0x142CBAD51 / 0x142CBAD5E | `4C 8B B6 B0 A2 56 00` / `E8` -> 0x142C35CC0 / `41 FF 90 C0 00 00 00` | contrib: the level-up list's consumer (StartLevelUp vt+0xC0 with the reached level) |
| 0x142CF9968 / 0x142CF9C5C | `E8` -> AddOwn / `E8` -> QueueLevelUp | contrib: the terminal commit 0x142CF98E0 |
| 0x142D32B92 | `E8` -> AddRepair | contrib: the repair commit 0x142D32AA0 |
| 0x142CA2D30 | `48 83 EC 48 8B 81 84 00 00 00 BA 01 00 00 00 4C 8B 81 90 00 00 00` (= base vtable 0x143DD9408 +0x140) | contrib: the durability repair |
| 0x142D3C870 / 0x142D2DD00 | `48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8` (its E8 -> ExportInfo) / `48 8B C4 53 48 81 EC A0 00 00 00 48 89 70 10 48 8B D9 48 89 78 18` | ladder (W3): the ladder's ExportInfo / InitFromInfo (vtable 0x143DE6208 +0x48 / +0x50) |
| 0x142D24470 / 0x142D24530 | `48 89 5C 24 08 57 48 83 EC 30 48 8B FA 48 8B D9 E8` (-> ExportInfo) / `48 89 5C 24 10 57 48 83 EC 20 48 8B DA 48 8B F9 E8` (-> InitFromInfo) | ladder: the rope's ExportInfo / InitFromInfo (vtable 0x143DE5E98 +0x48 / +0x50) |
| 0x1428D82A9 | `E8 ?? ?? ?? ?? 48 8B 4B 38 33 D2 8B 84 24 98 01 00 00 89 81 18 0B 00 00` (-> EnqueueInfo) | ladder: the anchor commit's EnqueueInfo call |
| 0x142C38200 / 0x1418F9610 | `40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D8 48 81 EC 38 01 00 00` / `48 8B 49 08 E9`; 0x1428B200D `E8` -> 0x142C38200 | ladder: the placement check (dev placements) and its reasons' free; the build preview calls it |
| 0x142D2E340 / 0x142D2E5B0 / 0x142D24610 | `48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 20 FE FF FF 48 81` / `48 8B C4 53 57 41 54 41 57 48 81 EC B8 0A 00 00` / `48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D 6C 24 80 48 81 EC 80 01 00 00` (ladder / rope vtables +0x60 checked) | ladder (detours): the ladder's re-check, its ground re-attach, the rope's re-check |
| 0x142CA3E00 | `40 53 56 41 56 B8 10 F5 00 00 E8` | ladder (detour): Remove (cp, keep cargo): its caller for ladders / ropes |
| 0x142CE5A40 / 0x142CE5A80 | `48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 0F B6 83 20 05 00 00` (E8 -> ExportInfo) / `48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 0F B6 83 78 05 00 00` (E8 -> InitFromInfo) | zipline (W4): the zipline's ExportInfo / InitFromInfo (vtable 0x143DDCC38 +0x48 / +0x50) |
| 0x142CE5520 | `4C 8B DC 57 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 58` (= vtable 0x143DDCC38 +0x10) | zipline: the zipline's activation (links it to the ziplines in range) |
| 0x142CE5575 / 0x142CE5590 | `41 B1 09 4C 89 64 24 48 4C 89 64 24 50 C5 FA 10 80 AC 04 00 00` / `E8` -> 0x142CAA000 | zipline: the activation collects category 9 within the config's range (+0x4AC) with 0x142CAA000 |

Construction manager fields used: +0x70 config, +0x80 pool (3140 x 0x710), +0x56A258 / +0x56A260 live list, +0x56A278
creation queue, +0x56A5B8 its lock, +0x572D34 Player id counter. CP fields: +0x30 id, +0x34 type, +0x35 category, +0x36
state, +0x40 position, +0x84 level, +0x88 entity, +0xAA / +0xAB / +0xAD flags, +0xB2 net type, +0xB8 server id, +0xD8
durability, +0x210 own materials, +0x248 others' sum. Per-category config +0x90 = the Player limit. W2: manager +0x56A2A8 /
+0x56A2B0 the level-up list (count / ids); CP +0xA8 bit 7 a level-up (or the first build-up) runs, +0x228 / +0x230 the
contributor list, +0x260 repair int[6], +0x458 the others' stamp; config +0x98 the last level, +0xA0 float[5] the
durability per level, +0x210 float[6] the repair weights.

## Hidden scenarios (W1)

All: `-SeedSaves complete-100`; two-game ones `-GuestSeed profile-only` and the launcher's own Local test files
(`launcherini local-test`, which now carry `world_sync=1`), except `two-games-world-off` (its own `ini` lines).

| Scenario | Proves | Run |
|---|---|---|
| `world-census` (solo) | byte checks, the gate ON, `world_dump` of the 100% save (1125 structures, 17 shared, Player counter 165), record round trips EQUAL, a minute of OTGC requests: reads only | `20261121-150217` PASS |
| `world-build-solo` (solo) | `dev_build` with the build-up (postbox: state 2, level 0 -> 1 after ~18 s, offline 1, server id empty, round trip EQUAL), the gate dropped its RegisterNew, a watchtower without animation, the dismantle | `20261121-151253` PASS |
| `two-games-world-build` | host build -> guest mirror (exact id, anim, printer) with screenshots of both screens; guest build -> held -> GBUILD -> host created -> ADD -> confirmed; counts 1 / 1 in both worlds; host dismantle -> REM manual (the guest's copy plays the dismantle); guest dismantle -> REMREQ -> host removes; counts 0 / 0 | `20261121-152649` PASS (first `151607` PASS) |
| `two-games-world-persist` | one build each way, the host's save (its own autosave) and an in-world load of it; the guest follows: each structure exactly once in each world after the load (ids 10166 10167, offline 2), the guest's second join `bound 19, created 0` | `20261121-152057` PASS |
| `two-games-world-off` | the guest's byte site failed (`world_test_fail=NewId`): one-line reason, the host shares nothing, the host's build exists only in its world, vehicles still bind | `20261121-153731` PASS |
| `world-pcc-probe` (solo) | calibration only: the quickstart Sam holds the PCC (level 2, postbox preview, "LT: Adjust Structure Location"); RT / RB / A / X / LT+RT did not confirm a build | `20261121-152514` (probe) |

Every run: saves unchanged, game folder restored, no violation, 0 cloud uploads, 0 game server writes, both games exit 0.

## Hidden scenarios (W2)

Same seeds and files. Screenshots (cropped and enlarged) are in the round's notes, not in the repo.

| Scenario | Proves | Run |
|---|---|---|
| `world-contrib-solo` (solo) | the game's own machinery on a dev-built postbox: level 1 of 3, half of the 200 metals, the rest and the terminal's QueueLevelUp, Status 4 while the countdown runs, level 2 after ~17 s (screenshots `00..04`), a decay and a 20-metal repair (+4800 s) | `20261121-165554` (first `160128`) PASS |
| `two-games-world-contrib` | A the host's postbox: the host pays 100 metals (STATE: her invested 0 -> 100), she pays the other 100 (MATS #1, her level-up request swallowed; the host: applied, status 0, level-up queued; STATE: hers queued), the countdown on both screens, level 2 / invested 0/0/200 in both. B the guest's watchtower at `road-holo` (created by the host, mirrored): she pays 16 crystals + 200 metals (MATS), the host pays the rest with its own terminal step (STATE "a level-up runs" -> hers queued), level 2 in both. C the host's durability drop to 600 s reaches her (DUR), her 20-crystal repair (MATS repair) raises the host's (+4800 s) | `20261121-165659` (first `160615`) PASS |
| `two-games-world-contrib-persist` | both pay the whole remainder in the same step: invested 0/0/400 in both, one level-up per world (`level-ups queued: host 0 guest 1`); the host's save and an in-world load, the guest follows: the postbox once in each world, offline 1, level 2, invested 0/0/400, `snapshot #2 complete: bound 18, created 0`; a contribution after the load (0/0/500 in both) | `20261121-165913` (first `161158`) PASS |
| `two-games-world-contrib-ui` | the GUEST pays through the game's own upgrade UI with her virtual pad (X tap, START held: the structure menu, A: Select Materials, A, DPAD_RIGHT held to 32, A, A held: Confirm): 32 chiral crystals leave her inventory through the real terminal commit, `MATS 32/0/0/0/0/0` -> the host `applied 32/0/0/0/0/0` -> invested 32/0/0 in both; then the host pays the metals: level 2 in both | `20261121-170106` (first `162621`) PASS |
| `world-contrib-ui-probe` (solo) | calibration only: the prompts ("Hold START: Structure Menu", "Hold X: Activate Terminal"), the menu (Upgrade / Repair / Dismantle; the cursor starts on Dismantle when Upgrade and Repair have nothing to take), Select Materials, the amount pop-up, "Hold A: Confirm" | `20261121-161412` .. `162419` (7 probes) |

Regressions on this build: `two-games-world-build`, `two-games-world-persist`, `two-games-launcher-local-test`,
`two-games-hostsave-continue`, `two-games-ride-truck` (`20261121-170311`, `170515`, `170645`, `170857`, `171053`: all pass). Every run: saves unchanged, game folder restored, no
violation, 0 cloud uploads, 0 game server writes, both games exit 0.

## Hidden scenarios (W3)

Same seeds; the two-game ones on the launcher's own Local test files (`launcherini local-test`, now `world_sync=1` from the
Debug option). Screenshots (cropped, both screens) are in the round's notes, not in the repo.

| Scenario | Proves | Run |
|---|---|---|
| `world-ladder-census` (solo) | the `ladder` byte checks, the 100% save's Player ladders / ropes, `dev_ladder` on the road and `dev_rope` at a cliff edge through the game's placement check, each exactly once, offline 1, its record round trip EQUAL | `20261121-184025` PASS |
| `two-games-world-ladders` | the host's ladder -> `ADD` -> mirrored (exact id); the guest's ladder held -> `GBUILD` -> created by the host -> confirmed; the ladder fields equal in both worlds (`mode 0 lengths 9.65 / 9.65`), round trips EQUAL in both; **the guest collapses her ladder with the game's own prompt** (pad: Hold START) -> `REMREQ pickup` -> the host removes its copy (removal bits, no item); **the host collapses his** -> `REM pickup` -> hers goes; counts 0 / 0; a host rope mirrored, **the guest recovers it at its anchor** (Hold START) -> `REMREQ pickup` -> gone in both; a second host rope mirrored (count 1 / 1) | `20261121-185337` (final build; also `182124`, `184210`) PASS |
| `two-games-world-ladder-ui` | **the guest places a real ladder through the game's UI** (D-pad right tools, the ladder, LT hologram, RT place) at a cliff edge: her native commit held -> `GBUILD` (no dev flag) -> the host created it -> confirmed (count 1 / 1, level 2, durability from her item); she collapses it again (REMREQ pickup, 0 / 0); **the host places one through the same UI** (his vanilla commit, offline 0) -> `ADD` -> mirrored (1 / 1); the join check stays equal (no heal) | `20261121-185544` (final build; also `182336`, `180548`) PASS |
| `two-games-world-ladders-persist` | at a cliff edge a host rope, a host ladder and a guest ladder; the host's save and an in-world load (the guest follows): each exactly once in each world after the load, offline 1, same ids, the rope's fields equal; host `world ready (world 2): 34 shared structures` (31 before), guest `snapshot #2 complete: bound 34, created 0` | `20261121-182601` (first `180840`) PASS |
| `two-games-world-ladder-probe`, `world-ladder-ui-probe` | calibration only: the records in both worlds, the pick-up's flags (watch), the prompts, the tools radial | `20261121-173644`, `175325`, `175710` |

The climb: run `20261121-174141` (a development run of `two-games-world-ladders`): the guest grabs the host's rope ("Grab
Rope", X) and is on it in her world ("Release Rope", 28/30 m), the host above her. The final scenario's second rope step
takes screenshots only (whether she stands within reach of the anchor depends on where the placement check put it).

Regressions on this build (world sync now on by default with `host_save=1`: `two-games-hostsave-continue` and
`two-games-ride-truck` ran with it on, `[config] world_sync=1 (default: on with host_save=1)`): `two-games-launcher-local-test`,
`two-games-hostsave-continue`, `two-games-ride-truck`, `two-games-world-build`, `two-games-world-contrib`,
`two-games-world-off` (`20261121-182733`, `182949`, `183146`, `183444`, `183710`, `183924`: all pass). Batch `round5-w3`:
10 / 10; after a last fix (the guest's pick-up told apart from the game's removal by +0xAA bit 0x40 only: a rope's recovery
passes through state 3, run `20261121-184858` FAILED on it) batch `round5-w3-final` re-ran both pick-up scenarios: 2 / 2
(`185337`, `185544`). Every run: saves unchanged, game folder restored, no violation, 0 cloud uploads, 0 game server writes, both games
exit 0.

## Ziplines (W4)

Ziplines (`DSZiplineConstructionPoint`, Player type 22, category 9) are PCC structures (the printer, the build-up, levels to
pay into), so they ride the W1 / W2 pipeline: `wrules::IsSynced(type, ladders, ziplines)`, `Contributable(type,
ziplines)`; part bit3 in HELLO (both games must have it), session.ini `world_ziplines` (default on). The 100% save's 71
Player ziplines are bound at the first join with the rest (`bound 102` instead of 31).

**What the game does** (decompiled):

* **The record.** The zipline's ExportInfo 0x142CE5A40 / InitFromInfo 0x142CE5A80 add one byte: record +0x578 (a u32)
  <-> cp+0x520 (its update 0x142CE5710 sets it 0 / 2); inside the Record's +0x520..+0x5E3 range, so the wire did not
  change (`world_wire::SubclassValid`: the u32 <= 255).
* **The connections are NOT saved or sent.** The activation (vt+0x10 0x142CE5520, run when the creation queue makes the
  CP and when a save loads it) asks 0x142CAA000(mgr, area, pos, category 9, range = config +0x4AC) for every zipline of
  its area within range and links each **both ways** (the handle arrays cp+0x528: {i32 count, i32 cap, 0x18-byte entries,
  entry +0 = the other zipline + 0x18}). So the same set of ziplines links the same in both worlds; nothing to sync.
  Proven: `world_sync 16` lists the same 8 links for each new zipline in both worlds (10063 10143 10154 10161, the other
  new one, 104816 105881 106039), before and after the host's save and load.
* **Upgrades** (level 1 -> 2: chemicals 240 + special alloys 480; the range grows): the W2 contribution path (MATS /
  STATE); the level-up plays in both worlds.
* **The real PCC UI** (calibrated with the hidden env's pad, `world-pcc-ui-probe`..`5`): the PCC from the tools radial
  (D-pad right held, the right stick to about 7:30), the structure radial with D-pad DOWN held ("Switch Structure"; the
  right stick picks: about 4:30 the zipline, 10:30 the generator, 7:30 the watchtower, up the timefall shelter, down the
  safe house, left the cargo catapult, right the postbox), LT held = the hologram ("Adjust Structure Location", red with
  the reason when it cannot go: "Outside Permitted Construction Area" at `mknot-road`, "Steep Slope Detected", "Another
  Structure Detected Nearby", "Insufficient Chiral Bandwidth", "Another Structure Is Already Being Built"), RT = Build.
  W1's probe (`world-pcc-probe`) failed only because its spot is outside the permitted area.
* **One build at a time.** The game prints one structure at a time in a world: while the partner's new structure still
  prints in your world (about 18 s), your own hologram says "Another Structure Is Already Being Built" (as when you build
  two yourself). A zipline also keeps its distance from other structures.

**Logs**: as W1 with `type 22 (zipline)`, `printer 1`; `[world] zipline id N (hid H, zipline, net 1, level L, state S)
at (...): k link(s): ...` (`world_sync 16`); `[world] counters W4: ziplines shared (n in the census); not shared n;
durabilities sent n (ladders / ropes n)`; `[world] ziplines OFF: the partner's game does not share them (each world
keeps its own)`.

**The W3 gap closed: ladders' and ropes' durability.** The host's `DUR` (every 5 s, 2 % moves) now carries the shared
ladders and ropes too (`wrules::DurShared`); the guest writes cp+0xD8 as for the structures. Proven in
`two-games-world-ziplines`: the host's ladder set to 500 s -> `[world] guest: DUR 1: 1 applied, 1 moved more than 2 % (the
most: hid 10168 359996 -> 499 s)`.

## Hidden scenarios (W4)

Same seeds (`-SeedSaves complete-100`, two-game `-GuestSeed profile-only`), the launcher's own Local test files.
Screenshots (cropped, both screens) are in the round's notes, not in the repo.

| Scenario | Proves | Run |
|---|---|---|
| `world-zipline-solo` (solo) | the `zipline` byte checks, the save's ziplines and their links, `dev_build 22` with the build-up and printer (level 0 -> 1), its 7 links, round trip EQUAL, an upgrade (`dev_build_mats`: chemicals 240, alloys 480) to level 2 | `20261121-194314` (first `191554`) PASS |
| `two-games-world-ziplines` | join `bound 102` (the 71 save ziplines too); the host's zipline -> ADD -> mirrored (exact id, printer, build-up on both screens); the guest's -> held -> GBUILD -> created by the host (offline 1) -> confirmed; counts 2 / 2, the same 8 links in both worlds; **the guest pays the host's zipline's upgrade** (MATS -> applied, level-up queued -> STATE -> hers queued): level 2 in both; a ladder's durability (DUR); the guest dismantles the host's (REMREQ), the host the guest's (REM manual): 0 / 0 | `20261121-194441` (first `191730`) PASS |
| `two-games-world-ziplines-persist` | a zipline each way, the host's save (its own autosave) and an in-world load (the guest follows): ids 10166 10167 exactly once in each world, offline 2, level 1, the same links; host `world ready (world 2): 104 shared structures`, guest `snapshot #2 complete: bound 104, created 0` | `20261121-194739` (first `192131`) PASS |
| `two-games-world-zipline-ui` | **both players build a zipline through the game's real PCC UI** (pad): the host's vanilla commit (offline 0) -> ADD -> mirrored; the guest's native commit held -> GBUILD (no dev flag) -> created by the host (offline 1) -> confirmed; each exactly once in both worlds, both linked to each other in both worlds | `20261121-194945` (first `193741`) PASS |
| `world-pcc-ui-probe`..`world-pcc-ui-probe5` (solo) | calibration only: the tools radial, the structure radial, the holograms' reasons; probe 5 built a real generator (`dropped RegisterNew of structure 10166 (generator)`) | `20261121-192412`, `192540`, `192719`, `192856`, `193025` |

Regressions on this build: `two-games-world-ladders`, `two-games-world-contrib`, `two-games-world-build`,
`two-games-world-off`, `two-games-launcher-local-test`, `two-games-hostsave-continue`, `two-games-ride-truck`
(`20261121-195208`, `195412`, `195631`, `195839`, `195949`, `200159`, `200401`: all pass). Batch `round5-w4`: 11 / 11.
Every run: saves unchanged, game folder restored, no violation, 0 cloud uploads, 0 game server writes, both games exit 0.

## Not shared yet (after W4)

What else a player changes in the world, and why it is not shared:

* **Signs** (the "Create Sign" input action 0x5E; `DSSignboardComponent`, `DSNetSignboard`, `OnlineSignboard`): not a
  construction point. Their entity is set up by 0x142D4C4B0 from a record of a sign list (the sign type a char at +0x10:
  'b' / 'c' / 'f' / 'Z' ...), with an online sign path (`DestroySignboardOnline`) beside it. The create / remove calls,
  the list's owner and its save section are not decoded yet; a mirror needs its own census, create path and gate (signs
  are uploaded like structures). Next: the sign list and its create / destroy calls (start from 0x142D4C4B0's callers and
  the 0x5E action's handler), then the W1 pattern.
* **Dropped / placed cargo and locker / postbox contents** (`DSBaggage2Manager`, `CreateBaggage` 0x142CD1450,
  `DeleteBaggage`, `AddBaggageToDeliveryPoint` 0x142CD1350): each player's inventory is their own (the guest's is her
  sandbox copy), so sharing cargo means moving items between two inventories: a pick-up race (both pick up the same cargo
  = a duplicate), mission cargo ids and the order system. Not safe without an ownership protocol (a host-held cargo table
  with pick-up arbitration); left out. A structure's stored-cargo list (record +0x670) is never copied, so a mirror never
  duplicates cargo.
* **Auto-paver roads** (Stage type 36, `DSRoadRebuilderConstructionPoint`, 52 in the 100% save): a road's progress lives
  in the highway system (its own save section and server sync: the update 0x142CEBE50 pulls others' contributions from it
  into cp+0x524 and AddOthers; `devoteHighwayResources` uploads). Sharing it needs the highway system decoded and its
  uploads gated; not done. Contributions to auto-pavers stay in each world.
* **A refund for a refused guest build**: still no inventory-add path (a refused GBUILD loses her PCC item; only malformed
  or out-of-scope requests are refused).

## Not in W1 / W2 / W3 (gaps)

* W1's runs build through `dev_build` (W4 calibrated the real PCC input: section "Ziplines (W4)"; W1's probe above failed
  only because its spot is outside the permitted construction area); `dev_build` runs the commit's own steps (InitInfo, NewId, owner, +0x645 / +0x623, EnqueueInfo **through the
  detour** on the guest, SetLastBuilt). A real commit takes the same detours (the commit calls 0x142C47AF0 / 0x142CB5280,
  byte-checked at 0x1428D8767 / 0x1428D87F2).
* W2: roads (the auto-paver: `AddOthers`' road branch, the highway system [0x147C173B0] and its `devoteHighwayResources`
  upload) are not shared; Net structures (other players' online ones) keep their own numbers in each world; the guest's
  share shows as "others" on her terminal after the host's STATE (the session ledger of the design is not built); a
  guest payment the host refuses (guard, removed meanwhile) is lost from her inventory (no refund path, W4); the
  contributor list (+0x228, names) is not synced; a server refresh of +0x248 on a host-uploaded structure (design risk
  7) is not watched. The real UI path is proven for the guest's upgrade payment; the repair UI and the host's own UI
  payment run through the same commits but only dev-driven in the runs. The join check still ignores levels (STATE for
  the changed structures and DUR cover them).
* W3: a ladder's / rope's durability was not shared (W4: it is now, by DUR). The pick-up is
  told apart from the game's own removals by the re-check detours, the removal bits (a construction point the census still
  sees with them is the game's removal) and the durability left; round 5 adds the circumstances (a world in the census,
  at most two at once, her body within 20 m). A removal by a path the mod does not know, next to her body, that skips the
  removal bits and the census would still count as her pick-up and remove the host's ladder too (positive evidence from
  the pick-up action itself is the remaining step). The climbing anchor's real UI placement (LT / RT at an edge) is calibrated (the prompts) but only
  dev-driven in a run; the guest's rope REMREQ and the host's ladder REM ran through the real "Hold START" prompts. The
  launcher option is proven by unit tests and the hidden runs' Local test files (default on), not by a click.
* Roads, lockers, signs: see "Not shared yet (after W4)".
* An evicted structure (`REM evicted`) and a decay (`dev_build_decay`) are implemented but not proven in a run.
* A game whose world sync is wanted but off (a failed byte check) sends no HELLO; the partner shares nothing (it never
  hears `on`), which is safe but logs no "the partner is off" line on the other side.
* `world_anim=bits` is not built (falls back to `645`); `645` sends the mission event (orders "build X" complete) and
  counts build stats on the host for the guest's builds (design risk 1 and 3).
