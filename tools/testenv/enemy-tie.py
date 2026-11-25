"""Round 23 M1 gate (docs/enemy-sync.md 14.12): no replay of a tied-up MULE later than 0.5 s after its down outcome reached
the sender (two-games-enemy-tie, two-games-enemy-tie-host).

    python -I tools/testenv/enemy-tie.py <run folder> [--reverse] [--id XXXXX]

The MULE: --id, else the sender's first "[enemy] restrain watch id=XXXXX opened: ... [enemy_restrain test]" line, else 02029.

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest). The sender is the game
that tied the MULE up and replays its shots: the guest (default; two-games-enemy-tie) or the host (--reverse;
two-games-enemy-tie-host). In the sender's log:
  - T, the moment the down outcome reached it: the first of
      the guest: "[enemy] OUTCOME id=XXXXX none -> restrained ver" (the host's OUTCOME, GuestOutcome),
      the host:  "[enemy] restrain HIT #n id=XXXXX: her copy is restrained now (SEN1)",
      either:    "[shots] flushed n replays of id=XXXXX (down: ...)" (the flush itself);
  - every "[shots] enemy replay #n: shooter XXXXX" line (hidden runs log the first 200 one by one, then every 20th).
It FAILS (exit 1) when:
  - T is not found (the outcome never reached the sender);
  - the sender has no "[shots] flushed n replays of id=XXXXX" line;
  - a replay of XXXXX is logged later than T + 0.5 s;
  - (the guest only) her mirror of XXXXX lost its lease ("[enemy] lease released id=XXXXX") or was "given back alive" after T
    and before "[enemy] host Sam back: resuming" (the tie-up held through the host's Sam down).
It notes (no fail) when the replay counter passed 200 (sampled lines: a late replay between two logged ones is not seen).
Read only; no game.
"""
import re
import sys

STAMP = re.compile(r"^\[\s*([0-9]+\.[0-9]+)\]")
REPLAY = re.compile(r"\[shots\] enemy replay #(\d+): shooter ([0-9A-F]{5})")
LIMIT_S = 0.5
BACK = re.compile(r"\[enemy\] host Sam back: resuming")


def lines(path):
    with open(path, "rb") as f:
        for raw in f:
            yield raw.decode("utf-8", "replace")


def stamp(line):
    m = STAMP.search(line)
    return float(m.group(1)) if m else None


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    run = argv[0].rstrip("/\\")
    reverse = "--reverse" in argv[1:]
    mid = None
    if "--id" in argv[1:]:
        i = argv.index("--id")
        if i + 1 >= len(argv) or not re.fullmatch(r"[0-9A-Fa-f]{1,5}", argv[i + 1]):
            print(__doc__)
            return 2
        mid = "%05X" % int(argv[i + 1], 16)
    path = run + ("/game/stranded_together.log" if reverse else "/guest/game/stranded_together.log")
    who = "the host" if reverse else "the guest"
    if mid is None:
        watch = re.compile(r"\[enemy\] restrain watch id=([0-9A-F]{5}) opened: .*\[enemy_restrain test\]")
        mid = next((m.group(1) for m in map(watch.search, lines(path)) if m), "02029")
    down = re.compile(
        (r"\[enemy\] restrain HIT #\d+ id=%s: her copy is restrained now \(SEN1\)" if reverse else r"\[enemy\] OUTCOME id=%s none -> restrained ver") % mid)
    flush = re.compile(r"\[shots\] flushed (\d+) replays of id=%s \(down: ([^;]*)" % mid)
    fails = []
    t_down = None
    flushes = []
    replays = []  # (t, n)
    max_n = 0
    lost = []  # (t, line): the guest's lease releases of XXXXX
    t_back = None
    lease = re.compile(r"\[enemy\] (lease released id=%s|mirror id=%s released while \w+ on the host: given back alive)" % (mid, mid))
    for line in lines(path):
        t = stamp(line)
        if t is None:
            continue
        if down.search(line) and t_down is None:
            t_down = t
            continue
        m = flush.search(line)
        if m:
            flushes.append((t, int(m.group(1)), m.group(2)))
            continue
        if not reverse and lease.search(line):
            lost.append((t, line.strip()[:160]))
            continue
        if not reverse and t_down is not None and t_back is None and BACK.search(line):
            t_back = t
            continue
        m = REPLAY.search(line)
        if m:
            max_n = max(max_n, int(m.group(1)))
            if m.group(2) == mid:
                replays.append((t, int(m.group(1))))
    t_first = min([x for x in [t_down] + [f[0] for f in flushes] if x is not None], default=None)
    print("%s: %s down at %s s (outcome line %s; flushes %s); replays of %s logged %d (counter up to %d)" % (
        who, mid, "%.3f" % t_first if t_first is not None else "-", "%.3f" % t_down if t_down is not None else "none",
        ["%.3f s: %d (%s)" % f for f in flushes], mid, len(replays), max_n))
    if t_first is None:
        fails.append("the down outcome of %s never reached %s" % (mid, who))
    if not flushes:
        fails.append("%s never flushed the replays of %s" % (who, mid))
    if t_first is not None:
        before = [r for r in replays if r[0] <= t_first]
        late = [r for r in replays if r[0] > t_first + LIMIT_S]
        print("%s: replays of %s before the outcome %d, in the 0.5 s after it %d, later %d" % (
            who, mid, len(before), len([r for r in replays if t_first < r[0] <= t_first + LIMIT_S]), len(late)))
        for t, n in late[:5]:
            fails.append("replay #%d of %s at %.3f s: %.3f s after its down outcome (limit %.1f s)" % (n, mid, t, t - t_first, LIMIT_S))
    if not reverse and t_down is not None:
        end = t_back if t_back is not None else float("inf")
        held = [x for x in lost if t_down <= x[0] <= end]
        print("the guest: lease releases of %s in all %d, from her OUTCOME to the host's Sam back (%s) %d" % (
            mid, len(lost), "%.3f s" % t_back if t_back is not None else "none", len(held)))
        for t, l in held[:3]:
            fails.append("her mirror of %s lost its lease at %.3f s while tied up: %s" % (mid, t, l))
    if max_n > 200:
        print("note: the replay counter reached %d: past #200 only every 20th replay is logged (a late one between them is not seen)" % max_n)
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: %s flushed %s's replays and replayed none of it later than %.1f s after its down outcome" % (who, mid, LIMIT_S))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
