# Testing milestone 1

Every step writes `<game>\stranded_together\stranded_together.log`. Read it after each
run; each install step logs one line saying whether it worked and why not.

## 0. Offline checks (no game)

```bash
ctest --test-dir build -C Release --output-on-failure
```

- `st_net_tests`: pose codec, rotation round-trips, DST1 join codes (incl. typo detection),
  a full link v2 handshake + pose exchange between two in-process sessions, and a
  wrong-secret session that must never connect.
- `st_fakepeer_selftest`: two `st_fakepeer` processes connect over UDP on 127.0.0.1 and
  exchange ~30 poses/s for 6 s using the same `link.cpp` the game DLL uses.

## 1. Solo in-game test (echo)

```powershell
.\tools\deploy.ps1 -Session echo
```

Start DS from Steam, load a save, walk around.

Expected log lines, in order:

```
[mod] Stranded Together 0.1.0+xxxxxxx loaded into ds.exe
[config] role=echo ...
[game] GetLocalDSPlayerEntity         at ds.exe+0x2757A30 (known rva)      <- x7, all found
[mod] frame pump hooked
[sync] first tick on thread N
[sync] local player entity 0000000000000000 -> 00000xxxxxxx               <- after the save loads
[partner] echo resource ...: <type> -> builds <EntityType>; N components: ...  -> OK
[partner] spawned ...: <type> flags=... (visible=1 hidden=0 asleep=0) model=... ; N components: ...
[sync] partner spawned from the echo resource: entity=... at (x, y, z) flags now 0x... (set visible ok, wake ok)
[sync]   Sam flags 0x... -> 0x...                                          <- must NOT say "Sam was put to sleep"
[sync] partner +1s: flags=0x... at (...) target (...)                      <- partner position vs where we put it
[sync] tick rate: 60.0 Hz over 10 s                                        <- confirms the hook is per-frame
```

What to look for, and what each failure means:

| Observation | Meaning / next step |
|---|---|
| `[game] ... NOT FOUND` | Different ds.exe build; signatures need updating (`tools/re/mksigs.py`). |
| no `first tick` | The RunCallbacks wrapper is not called per frame in this build; pick another hook. |
| `tick rate` far from the frame rate | Wrapper not per-frame (e.g. 1–10 Hz); movement will look choppy. |
| `[partner] ... -> REFUSED: ...` | The echo resource failed a safety check (player/AI parts); nothing is spawned. Send the log. |
| `echo config not loaded yet` forever | The echo data is not loaded in this area/mode; send the log. |
| `not spawning: the game's echo teardown condition is set` | Expected briefly; if it never clears, that condition is misread. Send the log. |
| `partner spawn FAULTED inside CreateEntity` | CreateEntity rejected the resource; spawning stops for this run. Send the log. |
| `trace ... awake=0` while you are walking | The sleep flag reading is wrong for this build; the partner will never spawn. Send the log. |
| `<-- Sam was put to sleep!` | The player-system trap fired despite the gate: quit, set `partner_activate=0`, retry and send both logs. |
| Spawned but invisible | Try `partner_activate=0` vs `1` and compare the `flags` in the log (bit 0x100 = asleep). |
| `partner +1s` position far from target, partner snaps back | The clone's DSPlayerMover overwrites our transform each frame; next step is driving the mover instead of the entity. |
| Partner copies your stick input / camera jumps / stamina drains twice | Its player components are live; expected risk of the clone approach (see docs/reverse-engineering.md). Report which. |
| `EXCEPTION in the per-frame sync` | Co-op disabled itself; the game keeps running. Send the log. |

## 2. One PC, full network path (UDP + fakepeer)

```powershell
.\tools\deploy.ps1 -Session udp-host
# start DS, load a save, then:
.\build\Release\st_fakepeer.exe --role guest --local 27556 --remote 27555
```

The fakepeer prints `link=connected` and your position; in game the partner circles you
(3 m radius, 8 s per lap). Allow `st_fakepeer` and `ds.exe` through Windows Firewall
if asked (loopback normally needs nothing).

## 3. Two PCs over Steam

Both players: put `StrandedTogether.exe` and `winhttp.dll` in the DS folder (next to
`ds.exe`). Host: open the launcher → Host → send the copied `DST1-…@…` code. Guest:
Join → paste → Play. Both games start through Steam. The launcher's In game page shows
the link state (`connected`, `relay`/`direct`, RTT) from `link-status.json`.

The two builds must match: the handshake compares build ids and refuses a mismatch.

## Uninstall

```powershell
.\tools\deploy.ps1 -Uninstall
```
