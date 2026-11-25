"""Round 23 M1 gate, the trucks (docs/enemy-sync.md 14.12): the MULE trucks held still in each game (two-games-truck-hold).

    python -I tools/testenv/truck-hold.py <run folder> [--tags A B] [--max-m M]

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest):
  - the truck census lines "[vshare] trucks tag=N tNN truck 0xXXXXXX: MULE truck ...; at (x, y, z) ..." (vehicle_share 9),
  - the host's "[vshare] status: host ON; ...; poll ok (N in use)" and the guest's "[vshare] guest: table T, bound B" lines.
It FAILS (exit 1) when, on either game:
  - census tag A (default 1) lists no MULE truck, or tag B (default 2) lacks one tag A listed;
  - a MULE truck moved more than M m (default 0.5) between tags A and B (the calm minute: no parked teleport); a truck empty
    at tag A and "MULE-driven" at tag B is skipped (a camp MULE drove it off: the game's own drive, not a teleport);
  - the guest's table is not fully bound in its last line (B != T), or the host's last "poll ok (N in use)" differs from the
    guest's last bound count B.
Read only; no game.
"""
import math
import re
import sys

CENSUS = re.compile(r"\[vshare\] trucks tag=(\d+) t\d+ truck 0x([0-9A-F]{6}): (MULE truck[^;(]*).*? at \(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\)")
HOST_POLL = re.compile(r"\[vshare\] status: host ON; .*poll ok \((\d+) in use\)")
GUEST_BOUND = re.compile(r"\[vshare\] guest: table (\d+), bound (\d+), mirrors in flight (\d+)")


def lines(path):
    with open(path, "rb") as f:
        for raw in f:
            yield raw.decode("utf-8", "replace")


def census(path):
    tags = {}
    for line in lines(path):
        m = CENSUS.search(line)
        if m:
            tags.setdefault(int(m.group(1)), {})[m.group(2)] = (m.group(3).strip(), tuple(float(m.group(k)) for k in (4, 5, 6)))
    return tags


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    run = argv[0].rstrip("/\\")
    ta, tb, max_m = 1, 2, 0.5
    try:
        if "--tags" in argv:
            i = argv.index("--tags")
            ta, tb = int(argv[i + 1]), int(argv[i + 2])
        if "--max-m" in argv:
            max_m = float(argv[argv.index("--max-m") + 1])
    except (IndexError, ValueError):
        print(__doc__)
        return 2
    fails = []
    logs = {"host": run + "/game/stranded_together.log", "guest": run + "/guest/game/stranded_together.log"}
    for who, path in logs.items():
        tags = census(path)
        a, b = tags.get(ta, {}), tags.get(tb, {})
        if not a:
            fails.append("%s: census tag %d lists no MULE truck" % (who, ta))
            continue
        for tid, (what, pa) in sorted(a.items()):
            if tid not in b:
                fails.append("%s: MULE truck 0x%s of tag %d missing in tag %d" % (who, tid, ta, tb))
                continue
            what_b, pb = b[tid]
            d = math.dist(pa, pb)
            print("%s: MULE truck 0x%s %s -> %s, moved %.2f m between tags %d and %d" % (who, tid, what, what_b, d, ta, tb))
            if "driven" in what_b and "driven" not in what:
                # a camp MULE got in and drove it off in this game (the game's own patrol, run 163849): not a parked teleport
                print("%s: MULE truck 0x%s: a non-player drives it at tag %d (not held: the game's own drive)" % (who, tid, tb))
                continue
            if d > max_m:
                fails.append("%s: MULE truck 0x%s moved %.2f m between tags %d and %d (limit %.1f m)" % (who, tid, d, ta, tb, max_m))
        for t in sorted(tags):
            if t not in (ta, tb):
                print("%s: census tag %d: %s" % (who, t, ", ".join("0x%s %s at (%.1f, %.1f, %.1f)" % ((k, v[0]) + v[1]) for k, v in sorted(tags[t].items()))))
    poll = [int(m.group(1)) for m in map(HOST_POLL.search, lines(logs["host"])) if m]
    bound = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in map(GUEST_BOUND.search, lines(logs["guest"])) if m]
    print("host poll counts %s; guest (table, bound, in flight) %s" % (poll, bound))
    if not poll:
        fails.append("host: no \"poll ok (N in use)\" status line")
    if not bound:
        fails.append("guest: no \"guest: table T, bound B\" line")
    if poll and bound:
        t, b, _ = bound[-1]
        if b != t:
            fails.append("guest: table %d, bound %d in her last line" % (t, b))
        if poll[-1] != b:
            fails.append("the host's last poll count %d differs from the guest's bound count %d" % (poll[-1], b))
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: every MULE truck held within %.1f m on both games; the guest bound %d = the host's %d in use" % (max_m, bound[-1][1], poll[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
