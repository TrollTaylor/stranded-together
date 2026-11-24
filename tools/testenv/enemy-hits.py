"""Enemy sync P2 (docs/enemy-sync.md 9): check the HITs of one two-games run (two-games-enemy-hit).

    python -I tools/testenv/enemy-hits.py <run folder>

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest):
  - the guest's "[enemy] HIT sent #n id=XXXXX ... kind K ..." lines (her HITs, by hit_seq),
  - the host's "[enemy] HIT applied #n id=XXXXX" and "[enemy] HIT refused #n id=XXXXX ...: <reason>" lines,
  - the host's "[enemy] OUTCOME id=XXXXX none -> faint (... cause guest ...)" line,
  - the guest's "[enemy] lease released id=XXXXX" lines and her "[enemy] outcome id=XXXXX faint" table line (enemy_sync 4).
It FAILS (exit 1) when:
  - no HIT was sent, or none of kind bullet (her own rounds);
  - a sent hit_seq is missing on the host (neither applied nor refused with a reason), or one is applied twice;
  - a refusal names fault, receiver or world;
  - the host's fainted id (cause guest) is not the guest's HIT target (the id of most of her HITs);
  - her mirror of that id was released before her outcome table showed it (the mirror stayed frozen through the faint).
Read only; no game.
"""
import collections
import re
import sys

SENT = re.compile(r"\[enemy\] HIT sent #(\d+) id=([0-9A-F]{5}) attack 0x([0-9A-F]+) kind (\S+)")
APPLIED = re.compile(r"\[enemy\] HIT applied #(\d+) id=([0-9A-F]{5})")
REFUSED = re.compile(r"\[enemy\] HIT refused #(\d+) id=([0-9A-F]{5}) attack 0x[0-9A-F]+(?: kind \d+)?: (\w+)")
FAINT = re.compile(r"\[enemy\] OUTCOME id=([0-9A-F]{5}) none -> faint \(.*cause guest")
RELEASED = re.compile(r"\[enemy\] lease released id=([0-9A-F]{5})")
TABLE = re.compile(r"\[enemy\] outcome id=([0-9A-F]{5}) faint ver")
BAD = {"fault", "receiver", "world"}


def lines(path):
    with open(path, "rb") as f:
        for raw in f:
            yield raw.decode("utf-8", "replace")


def main(run):
    host = run.rstrip("/\\") + "/game/stranded_together.log"
    guest = run.rstrip("/\\") + "/guest/game/stranded_together.log"
    fails = []
    sent = collections.OrderedDict()
    kinds = collections.Counter()
    targets = collections.Counter()
    guest_lines = list(lines(guest))
    for line in guest_lines:
        m = SENT.search(line)
        if m:
            seq = int(m.group(1))
            if seq in sent:
                fails.append("hit_seq #%d sent twice" % seq)
            sent[seq] = m.group(2)
            kinds[m.group(4)] += 1
            targets[m.group(2)] += 1
    applied = collections.Counter()
    refused = {}
    faint = []
    for line in lines(host):
        m = APPLIED.search(line)
        if m:
            applied[int(m.group(1))] += 1
            continue
        m = REFUSED.search(line)
        if m:
            refused[int(m.group(1))] = m.group(3)
            continue
        m = FAINT.search(line)
        if m:
            faint.append(m.group(1))
    print("guest: %d HIT(s) sent (kinds %s), targets %s" % (len(sent), dict(kinds), dict(targets)))
    print("host: %d applied, %d refused %s; faint (cause guest): %s" % (sum(applied.values()), len(refused), dict(collections.Counter(refused.values())), faint))
    if not sent:
        fails.append("no HIT sent")
    if not kinds.get("bullet"):
        fails.append("no HIT of kind bullet (her own rounds)")
    for seq in sent:
        if applied[seq] == 0 and seq not in refused:
            fails.append("hit_seq #%d never reached the host (neither applied nor refused)" % seq)
        if applied[seq] > 1:
            fails.append("hit_seq #%d applied %d times" % (seq, applied[seq]))
        if seq in refused and applied[seq]:
            fails.append("hit_seq #%d both applied and refused" % seq)
    for seq, why in refused.items():
        if why in BAD:
            fails.append("hit_seq #%d refused: %s" % (seq, why))
    target = targets.most_common(1)[0][0] if targets else None
    if not faint:
        fails.append("no OUTCOME none -> faint (cause guest) on the host")
    elif target not in faint:
        fails.append("the host's fainted id(s) %s are not her HIT target %s" % (faint, target))
    # her mirror of the target: frozen from her first HIT until her outcome table showed the faint (no release in between)
    first = next((i for i, l in enumerate(guest_lines) if SENT.search(l) and SENT.search(l).group(2) == target), None)
    table = next((i for i, l in enumerate(guest_lines) if TABLE.search(l) and TABLE.search(l).group(1) == target), None)
    if target and first is not None:
        if table is None:
            fails.append("her outcome table never showed %s as faint" % target)
        else:
            for i in range(first, table):
                m = RELEASED.search(guest_lines[i])
                if m and m.group(1) == target:
                    fails.append("her mirror %s was released before her outcome table (line %d: %s)" % (target, i + 1, guest_lines[i].strip()[:160]))
                    break
    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1
    print("PASS: every HIT once on the host, the fainted id %s is her target, her mirror stayed frozen" % target)
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
