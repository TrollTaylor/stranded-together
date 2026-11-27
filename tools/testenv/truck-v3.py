"""Round 28 T1 gate, host-owned MULE trucks (docs/enemy-sync.md 14.21): the host's MULE truck is the same truck on both screens
(two-games-truck-v3-host, vehicle_mule_v3=1 on both games).

    python -I tools/testenv/truck-v3.py <run folder> [--tag N] [--strict-final]

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest):
  - the host's "[truckv3] v3 host: truck 0xT MULE 0xM drives it: DRIVE frames ..." and "... no longer drives it ..." lines,
  - the guest's "[vdrive] follower of truck 0xT attached #n" lines, her "[truckv3] v3 guest: follower of truck 0xT (the host's
    MULE drives it): ..." 5 s lines and "... over the follow (the host's MULE drove it): ..." detach lines,
  - the guest's enemy lines "[enemy] mirror id=X in a vehicle (" (the v2 release) and "seated in V3 truck" (the v3 verdict),
  - "owner guest (MULE" on either game (a MULE of the guest's game drives her copy),
  - the truck census of tag N (default 2; vehicle_share 9) on both games.
It FAILS (exit 1) when:
  - the host never sent a MULE-driven truck, or the guest's follower never attached to one it sent;
  - over the guest's follows (the detach summaries; a follow still attached at the end: its newest 5 s line) the placement
    error while the host's truck moved at >= 5 m/s has fewer than 30 ticks, a mean over 0.3 m or a max over 2.5 m;
  - more than 2 driven (far) teleports over those follows;
  - the guest released a mirror "in a vehicle", or no mirror was "seated in V3 truck" there;
  - an "owner guest (MULE" line on either game, or a "follower of truck ... refused" line on the guest;
  - at census tag N a truck both games list and the host lists parked ("MULE truck empty") is more than 0.5 m apart; a
    followed truck still MULE-driven on the host at tag N is checked by the guest's last 5 s max (<= 0.5 m) instead, and
    with --strict-final it must be parked (the MULE stopped driving it before tag N).
Read only; no game.
"""
import re
import sys

START = re.compile(r"\[truckv3\] v3 host: truck 0x([0-9A-F]{6}) MULE 0x([0-9A-F]+) drives it: DRIVE frames")
STOP = re.compile(r"\[truckv3\] v3 host: truck 0x([0-9A-F]{6}): MULE 0x[0-9A-F]+ no longer drives it \(([^)]*)\)")
ATTACH = re.compile(r"\[vdrive\] follower of truck 0x([0-9A-F]{6}) attached #(\d+)")
WINDOW = re.compile(r"\[truckv3\] v3 guest: follower of truck 0x([0-9A-F]{6}) \(the host's MULE drives it\): placement error mean ([0-9.]+) max ([0-9.]+) m "
                    r"over (\d+) ticks; at >= 5 m/s mean ([0-9.]+) max ([0-9.]+) m over (\d+); last 5 s max ([0-9.]+) m; (\d+) driven teleports")
OVER = re.compile(r"\[truckv3\] v3 guest: follower of truck 0x([0-9A-F]{6}) over the follow \(the host's MULE drove it\): at >= 5 m/s mean ([0-9.]+) "
                  r"max ([0-9.]+) m over (\d+) ticks; all mean ([0-9.]+) max ([0-9.]+) m over (\d+); (\d+) driven teleports")
RELEASE = re.compile(r"\[enemy\] mirror id=([0-9A-F]+) in a vehicle \(")
SEATED = re.compile(r"\[enemy\] mirror id=([0-9A-F]+) seated in V3 truck 0x([0-9A-F]{6})")
OWNER_GUEST = re.compile(r"owner guest \(MULE")
REFUSED = re.compile(r"\[truckv3\] v3 guest: follower of truck 0x([0-9A-F]{6}) refused")
# (the optional " (host's id differs)" between the id and ':' is group 7: such a truck is a FAIL, never skipped)
CENSUS = re.compile(r"\[vshare\] trucks tag=(\d+) t\d+ truck 0x([0-9A-F]{6})( \(host's id differs\))?: (MULE truck[^;(]*).*? at \(([-0-9.]+), ([-0-9.]+), ([-0-9.]+)\)")


def read(path):
    try:
        with open(path, "rb") as f:
            return [raw.decode("utf-8", "replace") for raw in f]
    except OSError:
        return None


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main(argv):
    if not argv or argv[0].startswith("-"):
        print(__doc__)
        return 2
    run = argv[0].rstrip("/\\")
    tag, strict = 2, "--strict-final" in argv
    try:
        if "--tag" in argv:
            tag = int(argv[argv.index("--tag") + 1])
    except (IndexError, ValueError):
        print(__doc__)
        return 2
    host = read(run + "/game/stranded_together.log")
    guest = read(run + "/guest/game/stranded_together.log")
    if host is None or guest is None:
        print("FAIL: a log is missing (%s)" % ("host" if host is None else "guest"))
        return 1
    fails, notes = [], []

    # the host's MULE-driven trucks; the line of the census tag (stops before it count as parked)
    sent, stop_line = {}, {}
    census_line_h = None
    for i, line in enumerate(host):
        m = START.search(line)
        if m:
            sent.setdefault(m.group(1), 0)
            sent[m.group(1)] += 1
        m = STOP.search(line)
        if m:
            stop_line[m.group(1)] = (i, m.group(2))
        m = CENSUS.search(line)
        if m and int(m.group(1)) == tag and census_line_h is None:
            census_line_h = i
        if OWNER_GUEST.search(line):
            fails.append("host: " + line.strip()[:200])
    print("host: MULE-driven trucks sent %s" % (", ".join("0x%s (%d starts)" % kv for kv in sorted(sent.items())) or "none"))
    if not sent:
        fails.append("host: no \"v3 host: truck ... MULE ... drives it\" line (V3 not live, or no MULE drove a truck near the guest)")

    # the guest's follows
    attached, follows, cur, seated, released, refused = set(), [], {}, set(), [], []
    for line in guest:
        m = ATTACH.search(line)
        if m:
            attached.add(m.group(1))
            cur[m.group(1)] = None  # a new follow: no 5 s line yet
        m = WINDOW.search(line)
        if m:
            cur[m.group(1)] = dict(fast_mean=float(m.group(5)), fast_max=float(m.group(6)), fast_n=int(m.group(7)), win_max=float(m.group(8)),
                                   tp=int(m.group(9)), truck=m.group(1), done=False)
        m = OVER.search(line)
        if m:
            follows.append(dict(fast_mean=float(m.group(2)), fast_max=float(m.group(3)), fast_n=int(m.group(4)), tp=int(m.group(8)), truck=m.group(1), done=True))
            last = cur.get(m.group(1))
            cur[m.group(1)] = dict(last, done=True) if last else None
        m = SEATED.search(line)
        if m:
            seated.add(m.group(1))
        m = RELEASE.search(line)
        if m:
            released.append(line.strip()[:200])
        m = REFUSED.search(line)
        if m:
            refused.append(line.strip()[:200])
        if OWNER_GUEST.search(line):
            fails.append("guest: " + line.strip()[:200])
    open_now = [v for v in cur.values() if v and not v["done"]]
    follows += open_now
    print("guest: followers attached %s; %d follows measured (%d still attached at the end)" % (", ".join("0x" + t for t in sorted(attached)) or "none",
                                                                                               len(follows), len(open_now)))
    if not attached & set(sent):
        fails.append("guest: the follower never attached to a truck the host's MULE drove")
    fast_n = sum(f["fast_n"] for f in follows)
    if fast_n < 30:
        fails.append("guest: %d ticks at >= 5 m/s over the follows (fewer than 30: the drive was not measured)" % fast_n)
    else:
        mean = sum(f["fast_mean"] * f["fast_n"] for f in follows) / fast_n
        mx = max(f["fast_max"] for f in follows)
        print("guest: placement error at >= 5 m/s mean %.3f max %.3f m over %d ticks" % (mean, mx, fast_n))
        if mean > 0.3:
            fails.append("guest: placement error mean %.3f m at >= 5 m/s (limit 0.3)" % mean)
        if mx > 2.5:
            fails.append("guest: placement error max %.3f m at >= 5 m/s (limit 2.5)" % mx)
    tp = sum(f["tp"] for f in follows)
    print("guest: %d driven teleports" % tp)
    if tp > 2:
        fails.append("guest: %d driven (far) teleports (limit 2)" % tp)
    print("guest: %d mirror(s) seated in a V3 truck, %d released in a vehicle" % (len(seated), len(released)))
    if released:
        fails += ["guest: in-a-vehicle release: " + r for r in released[:5]]
    if not seated:
        fails.append("guest: no mirror \"seated in V3 truck\" (the seated verdict was never shown)")
    fails += ["guest: " + r for r in refused[:5]]

    # the final positions (census tag N)
    def census(lines):
        out = {}
        for line in lines:
            m = CENSUS.search(line)
            if m and int(m.group(1)) == tag:
                out[m.group(2)] = (m.group(4).strip(), tuple(float(m.group(k)) for k in (5, 6, 7)), bool(m.group(3)))
        return out
    ch, cg = census(host), census(guest)
    if not ch or not cg:
        fails.append("census tag %d missing on the %s" % (tag, "host" if not ch else "guest"))
    # the followed truck (the MULE drove it and the guest's follower attached) must be in both censuses, under the same id
    for tid in sorted(attached & set(sent)):
        for side, c in (("host", ch), ("guest", cg)):
            if tid not in c:
                fails.append("truck 0x%s (MULE-driven, sent, followed) missing from the census tag %d on the %s" % (tid, tag, side))
            elif c[tid][2]:
                fails.append("truck 0x%s: the %s's census tag %d says the host's id differs (the final check cannot pair it)" % (tid, side, tag))
    for tid in sorted(set(ch) & set(cg)):
        what_h, ph, _ = ch[tid]
        d = dist(ph, cg[tid][1])
        parked = "driven" not in what_h
        stopped = tid in stop_line and census_line_h is not None and stop_line[tid][0] < census_line_h
        if tid in sent and not parked:
            last = [f for f in follows if f["truck"] == tid and not f["done"]]
            if strict:
                fails.append("truck 0x%s still MULE-driven on the host at tag %d (--strict-final: it must be parked)" % (tid, tag))
            elif not last:
                fails.append("truck 0x%s still MULE-driven on the host at tag %d and the guest has no follow of it attached" % (tid, tag))
            else:
                w = last[-1]["win_max"]
                print("truck 0x%s: still MULE-driven at tag %d (census %.2f m apart, taken at different moments): the guest's last 5 s max %.3f m" % (tid, tag, d, w))
                notes.append("truck 0x%s was still driven at the end: the 0.5 m parked check did not apply (the last 5 s max did)" % tid)
                if w > 0.5:
                    fails.append("truck 0x%s: the guest's last 5 s placement error max %.3f m (limit 0.5)" % (tid, w))
            continue
        print("truck 0x%s: %s on the host%s, %.2f m apart at tag %d" % (tid, what_h, " (its MULE stopped driving it: %s)" % stop_line[tid][1] if stopped else "", d, tag))
        if d > 0.5:
            fails.append("truck 0x%s: %.2f m apart at tag %d (limit 0.5)" % (tid, d, tag))
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    for n in notes:
        print("NOTE: " + n)
    print("PASS: the host's MULE truck followed on the guest (placement, teleports, seated mirrors, final positions)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
