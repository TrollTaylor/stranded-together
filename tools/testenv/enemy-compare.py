"""Enemy sync P1a (docs/enemy-sync.md 8): compare the host's and the guest's enemy census lines of one two-games run.

    python tools/testenv/enemy-compare.py <run folder> [--radius 250]

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest), takes the
"[enemy] tag=N eNNN id 0x... <class> state S guid G ... (x, y, z) xf|ent hdg H, D m; entity ..." lines of every census tag,
matches the enemies by GUID and prints, per tag:
  - mirrored: the enemies realized on the guest (state 2 with an entity) and within --radius of her Sam: the place
    difference to the host's (p50 / p95 / max, metres); the gate wants p95 within 1 m;
  - out of her world: the enemies unrealized on both (state 0) within 1000 m of her: the same numbers (enemy_place_far).
The gate FAILS (exit 1) when the two logs share no census tag, when a shared tag has no mirrored enemy, or when a tag's p95
is over 1 m. Read only; no game.
"""
import math
import re
import sys

LINE = re.compile(r"\[enemy\] tag=(\d+) e\d+ id 0x([0-9A-F]+) (\S+) state (-?\d+) guid ([0-9A-F]{32}|-) npc \S+ "
                  r"\((-?[0-9.]+), (-?[0-9.]+), (-?[0-9.]+)\) (?:xf|ent) hdg -?[0-9.]+, (-?[0-9.]+) m; (no entity|entity[^;]*)")


def read(path):
    tags = {}
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace")
            m = LINE.search(line)
            if not m:
                continue
            tag, gid = int(m.group(1)), m.group(5)
            if gid == "-":
                continue
            tags.setdefault(tag, {})[gid] = {
                "id": m.group(2), "state": int(m.group(4)), "pos": (float(m.group(6)), float(m.group(7)), float(m.group(8))),
                "dist": float(m.group(9)), "entity": m.group(10).startswith("entity") and "DEAD" not in m.group(10),
            }
    return tags


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    r = p * (len(v) - 1)
    i = int(r)
    return v[i] + (v[i + 1] - v[i]) * (r - i) if i + 1 < len(v) else v[i]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    run = args[0].rstrip("\\/")
    radius = 250.0
    if "--radius" in args:
        radius = float(args[args.index("--radius") + 1])
    host = read(run + "/game/stranded_together.log")
    guest = read(run + "/guest/game/stranded_together.log")
    worst = 0.0
    shared = sorted(set(host) & set(guest))
    empty = []
    if not shared:
        print("no census tag in both logs (host tags %s, guest tags %s)" % (sorted(host), sorted(guest)))
    for tag in shared:
        h, g = host[tag], guest[tag]
        mirrored, far, ids = [], [], []
        for gid, ge in g.items():
            he = h.get(gid)
            if not he:
                continue
            d = math.dist(he["pos"], ge["pos"])
            if ge["state"] == 2 and ge["entity"] and 0 <= ge["dist"] <= radius:
                mirrored.append(d)
                ids.append((d, ge["id"]))
            elif ge["state"] == 0 and he["state"] == 0 and 0 <= ge["dist"] <= 1000.0:
                far.append(d)
        ids.sort(reverse=True)
        if not mirrored:
            empty.append(tag)
        p95 = pct(mirrored, 0.95)
        worst = max(worst, p95)
        print("tag %d: matched %d; realized on the guest within %.0f m: %d, place difference p50 %.2f p95 %.2f max %.2f m%s; "
              "out of her world within 1000 m (both state 0; enemy_place_far): %d, p50 %.2f p95 %.2f max %.2f m"
              % (tag, len(set(h) & set(g)), radius, len(mirrored), pct(mirrored, 0.5), p95, max(mirrored) if mirrored else 0.0,
                 " (largest: " + ", ".join("%s %.2f" % (i, d) for d, i in ids[:3]) + ")" if ids else "",
                 len(far), pct(far, 0.5), pct(far, 0.95), max(far) if far else 0.0))
    ok = bool(shared) and not empty and worst <= 1.0
    why = ("" if ok else " (" + "; ".join(w for w in (
        "no shared tag" if not shared else "",
        ("no mirrored enemy at tag " + ", ".join(str(t) for t in empty)) if empty else "",
        ("p95 %.2f m" % worst) if worst > 1.0 else "") if w) + ")")
    print("gate (p95 within 1 m at every tag, every tag with a mirrored enemy): %s%s" % ("PASS" if ok else "FAIL", why))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
