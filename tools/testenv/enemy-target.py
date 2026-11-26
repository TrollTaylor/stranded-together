"""Round 27 M1 (docs/enemy-sync.md 15): the L1 probe's arm table and decision (two-games-enemy-l1-probe).

    python -I tools/testenv/enemy-target.py <run folder>

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest): every
"[probe] L1 result arm X id=XXXXX: key=value ..." line (the host: A, B, C, D, G; the guest: F, U21), the host's
"[probe] L1 decision: ..." line and the byte check / detour lines. Prints one row per arm and the decision, recomputed here by the
rules of native/mod/enemy_l1_rules.hpp (Decide):
  undecided B or C invalid (valid=0: its re-calm timed out, it saw her at its first sample, or her stimulus went away during it);
  FAIL      no PlayerVisual entry at her place in B and in C;
  undecided B ran without the goal reads (goals=0) or did not notice her (C did);
  P-notice  B's aim at her in < 70 % of its aim samples after the notice (or none);
  P-full    C noticed <= 4 s, turned to her <= 4 s after it, approached her; B fired >= 1 round, >= 60 % of them within 1.5 m of
            her chest (on each round's own line when the shot ring has it: rays=); D valid with the goal reads, switched to Sam and
            back to her, each <= 3 s (-2: n/a, her not its target before); and neither B nor C turned to / approached Sam or had
            the PlayerInfo daemon's centre mostly at Sam;
  P-nofire  not P-full and B fired no round;
  P-aim     otherwise.
Extras: melee (C's melee queries reached her body: PHIT path A, else B), crouch (F: a4 crouched < standing), G (whom the MULE
turned to after her hit), U21 (her frozen copy of 0202B held a PlayerVisual entry at her).
Exit 1 when an arm line is missing or the native decision differs from the recomputed one. Read only; no game.
"""
import os
import re
import sys

RESULT = re.compile(r"\[probe\] L1 result arm (\w+) id=([0-9A-F]{5})?:? ?(.*)$")
RESULT_F = re.compile(r"\[probe\] L1 result arm F: (.*)$")
DECISION = re.compile(r"\[probe\] L1 decision: ([A-Za-z-]+)")
CHECKS = re.compile(r"\[probe\] L1 (byte checks \(\d+\): .*|byte check FAILED: .*|detours \(read-only counts\): .*)$")
PAIR = re.compile(r"(\w+)=(\S+)")


def lines(path):
    if not os.path.exists(path):
        return
    with open(path, "rb") as f:
        for raw in f:
            yield raw.decode("utf-8", "replace").rstrip("\r\n")


def parse(path):
    arms, decision, checks = {}, None, []
    for line in lines(path):
        m = RESULT_F.search(line)
        if m:
            arms["F"] = dict(PAIR.findall(m.group(1)))
            continue
        m = RESULT.search(line)
        if m:
            d = dict(PAIR.findall(m.group(3)))
            d["id"] = m.group(2) or "-"
            arms[m.group(1)] = d
            continue
        m = DECISION.search(line)
        if m:
            decision = m.group(1)
            continue
        m = CHECKS.search(line)
        if m:
            checks.append(m.group(1))
    return arms, decision, checks


def num(d, k, default=-1.0):
    try:
        return float(d.get(k, default))
    except ValueError:
        return default


def aims_at_her(b):
    return num(b, "aim_n", 0) > 0 and num(b, "aim_her", 0) * 10 >= num(b, "aim_n", 0) * 7


def sam_pull(a):
    ds, dh = num(a, "daemon_sam", 0), num(a, "daemon_her", 0)
    return num(a, "turned_sam") >= 0 or num(a, "approached_sam", 0) > 0 or (ds > dh and ds > 0)


def c_full(c):
    n, t = num(c, "noticed"), num(c, "turned_her")
    return 0 <= n <= 4000 and t >= 0 and t - n <= 4000 and num(c, "approached_her", 0) > 0


def b_full(b):
    f = num(b, "fired", 0)
    return f >= 1 and num(b, "at_her", 0) * 10 >= f * 6


def d_full(d):
    s, h = num(d, "switch_sam"), num(d, "switch_her")
    return 0 <= s <= 3000 and 0 <= h <= 3000


def invalid(a):
    """An arm line with valid=0 (or its flags): the native Invalid()."""
    if not a:
        return False
    if a.get("valid") == "0":
        return True
    return any(num(a, k, 0) > 0 for k in ("pv_start", "calm_timeout", "stim_lost"))


def decide(b, c, d):
    if invalid(b) or invalid(c):
        return "undecided"
    if num(b, "noticed") < 0 and num(c, "noticed") < 0:
        return "FAIL"
    if b.get("goals") == "0" or num(b, "noticed") < 0:
        return "undecided"
    if not aims_at_her(b):
        return "P-notice"
    if (c_full(c) and b_full(b) and d_full(d) and not invalid(d) and d.get("goals") != "0" and not sam_pull(b)
            and not sam_pull(c)):
        return "P-full"
    if num(b, "fired", 0) == 0:
        return "P-nofire"
    return "P-aim"


COLS = [("arm", None), ("id", "id"), ("valid", "valid"), ("stim", "stim_start"), ("stim_n", "stim_n"), ("calm", "calm"), ("real", "realized"), ("notice", "noticed"),
        ("turn_h", "turned_her"), ("turn_s", "turned_sam"), ("turned", "turned"), ("appr_h", "approached_her"),
        ("appr_s", "approached_sam"), ("aim h/s/n", None), ("fired", "fired"), ("rays", "rays"), ("at h/s", None), ("pv h/s", None), ("lk h/s", None),
        ("daemon h/s", None), ("cp", "cp_alert"), ("tie", "tie"), ("switch s/h", None), ("melee", None), ("cls h/s", None), ("rec0 h/s", None)]


def row(name, a):
    out = []
    for title, key in COLS:
        if title == "arm":
            out.append(name)
        elif key:
            out.append(a.get(key, "-"))
        elif title == "aim h/s/n":
            out.append("%s/%s/%s" % (a.get("aim_her", "-"), a.get("aim_sam", "-"), a.get("aim_n", "-")))
        elif title == "at h/s":
            out.append("%s/%s" % (a.get("at_her", "-"), a.get("at_sam", "-")))
        elif title == "pv h/s":
            out.append("%s/%s" % (a.get("pv_her", "-"), a.get("pv_sam", "-")))
        elif title == "lk h/s":
            out.append("%s/%s" % (a.get("lk_her", "-"), a.get("lk_sam", "-")))
        elif title == "daemon h/s":
            out.append("%s/%s" % (a.get("daemon_her", "-"), a.get("daemon_sam", "-")))
        elif title == "switch s/h":
            out.append("%s/%s" % (a.get("switch_sam", "-"), a.get("switch_her", "-")))
        elif title == "melee":
            out.append("%s/%s" % (a.get("melee_her", "-"), a.get("melee_seen", "-")))
        elif title == "cls h/s":
            out.append("%s/%s" % (a.get("cls_her", "-"), a.get("cls_sam", "-")))
        elif title == "rec0 h/s":
            out.append("%s/%s" % (a.get("rec0_her", "-"), a.get("rec0_sam", "-")))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    run = argv[1]
    host, native, hchecks = parse(os.path.join(run, "game", "stranded_together.log"))
    guest, _, gchecks = parse(os.path.join(run, "guest", "game", "stranded_together.log"))
    for who, cl in (("host", hchecks), ("guest", gchecks)):
        for c in cl:
            print("%s: %s" % (who, c))
    table = [[t for t, _ in COLS]]
    for name in ("A", "B", "C", "D", "G"):
        if name in host:
            table.append(row(name, host[name]))
    if "U21" in guest:
        table.append(row("U21", guest["U21"]))
    widths = [max(len(r[i]) for r in table) for i in range(len(COLS))]
    for r in table:
        print("  ".join(v.ljust(widths[i]) for i, v in enumerate(r)))
    missing = [n for n in ("A", "B", "C", "D", "G") if n not in host]
    b, c, d = host.get("B", {}), host.get("C", {}), host.get("D", {})
    dec = decide(b, c, d)
    print()
    print("decision: %s (native: %s)%s" % (dec, native or "none", ("; missing arms " + "".join(missing)) if missing else ""))
    if c:
        print("melee: %s (her body in %s of %s query results)" % (
            "PHIT path A" if num(c, "melee_her", 0) > 0 else ("PHIT path B" if num(c, "melee_n", 0) > 0 else "no query"),
            c.get("melee_her", "-"), c.get("melee_seen", "-")))
    f = guest.get("F")
    if f:
        print("crouch: a4 standing %s crouched %s -> %s" % (f.get("a4_stand", "-"), f.get("a4_crouch", "-"),
                                                          "lower (send a4: U2 yes)" if f.get("lower") == "1" else "not lower (U2 fallback)"))
    g = host.get("G")
    if g:
        print("G: turned to %s; LK at her %s / Sam %s; CP alert at %s" % (g.get("turned", "-"), g.get("lk_her", "-"), g.get("lk_sam", "-"),
                                                                       g.get("cp_alert", "-")))
    u = guest.get("U21")
    if u:
        print("U21: her frozen copy %s her (PV at her in %s of %s realized samples)" % (
            "perceives" if num(u, "pv_her", 0) > 0 else "does not perceive", u.get("pv_her", "-"), u.get("realized", "-")))
    bad = bool(missing) or "F" not in guest or (native is not None and native != dec)
    if native is not None and native != dec:
        print("MISMATCH: the native decision differs from the recomputed one")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
