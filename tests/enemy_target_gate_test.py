"""Regression test of tools/testenv/enemy-target-gate.py (round 27 run 3 review): the gate's verdict rules on a trimmed run-3-format
fixture (tests/fixtures/enemy-target-gate/host.log.txt and guest.log.txt, laid out as a run folder: every item PASS; make_base.py
writes them; *.log is ignored by git) and on mutations of the host log, each asserting the gate's exit code and the status (and a
reason fragment) of the items it is about. Read only; no game.

    python -I tests/enemy_target_gate_test.py
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GATE = os.path.join(ROOT, "tools", "testenv", "enemy-target-gate.py")
FIX = os.path.join(ROOT, "tests", "fixtures", "enemy-target-gate")
HOST = os.path.join("game", "stranded_together.log")
GUEST = os.path.join("guest", "game", "stranded_together.log")
ITEM = re.compile(r"^(.+?)\s{2,}(PASS|FAIL|INVALID|INCONCLUSIVE)\s{2,}(.*)$")


def ts_of(line):
    return line[:line.index("]") + 1] + "[1] "


def insert_after(text, anchor, new_lines):
    """the new lines (each with the anchor line's time stamp) right after the first line containing `anchor`"""
    out, done = [], False
    for line in text.split("\n"):
        out.append(line)
        if not done and anchor in line:
            out.extend(ts_of(line) + n for n in new_lines)
            done = True
    assert done, "anchor not found: " + anchor
    return "\n".join(out)


def sub(text, old, new, count=1):
    n = text.count(old)
    assert n >= 1 and (count == 0 or n == count), "%r found %d time(s)" % (old, n)
    return text.replace(old, new)


def resub(text, pat, new):
    out, n = re.subn(pat, new, text)
    assert n >= 1, "pattern not found: " + pat
    return out


def watch(faces, her_m, age):
    return ("[probe] L1 watch id=0202B t=3.40: phase 5 ea4 0x42440000; faces her %d deg / Sam 20 deg; her %.1f m Sam 15.0 m; PV 1 at her 0.0 m "
            "id 00000 age %.1f; LK 1 at her; rounds 0" % (faces, her_m, age))


SWITCH = "[enemy] tgt id=0202B -> Sam (only visible)"
NO_CALLS = r"shoot 32: calls \d+ redirected \d+ points \d+ \(at her \d+ Sam \d+"


def no_calls(t):
    return resub(t, NO_CALLS, "shoot 32: calls 0 redirected 0 points 0 (at her 0 Sam 0")


# (name, host mutation, expected exit code, {item: (status, reason fragment or None)}, output fragments)
CASES = [
    ("base", None, 0, {"TB sticky her": ("PASS", None), "TF1 bit 32 on: no round at Sam": ("PASS", None), "bit 32 filter": ("PASS", None),
                       "bit 32 live": ("PASS", None), "TF0 bit 32 off: the actor branch at Sam": ("PASS", None)},
     ["op 11: CS id=02029: her not judged (a Sam arm)", "gate: PASS"]),
    # a switch to Sam in phase 0 FAILs whatever its rule (review: was INVALID for "only visible")
    ("TB switch, it turned off her", lambda t: insert_after(t, "[fixture] tb before op 7", [watch(82, 15.0, 0.9), SWITCH]), 1,
     {"TB sticky her": ("FAIL", "the turn redirect did not hold")}, ["gate: FAIL (TB sticky her)"]),
    ("TB switch, her dropped while it faces her", lambda t: insert_after(t, "[fixture] tb before op 7", [watch(10, 15.0, 1.2), SWITCH]), 1,
     {"TB sticky her": ("FAIL", "her dropped from its view while it faces her 10 deg")}, []),
    ("TB switch after she moved", lambda t: insert_after(t, "[fixture] tb before op 7", [watch(10, 45.0, 1.2), SWITCH]), 3,
     {"TB sticky her": ("INVALID", "she moved"), "TB phase 0: bit 32": ("INVALID", None)}, []),
    ("TB Sam out of its cone at op 7", lambda t: sub(t, "bearing 22 pitch 1.0; rule gun (her); sep 25", "bearing 59 pitch 1.0; rule gun (her); sep 25"), 3,
     {"TB sticky her": ("INVALID", "Sam not in its cone at op 7"), "TB phase 0: no round at Sam": ("INVALID", None)}, []),
    ("TB Sam step not placed", lambda t: sub(t, "-> ok: Enemy look: id 0202B: snap: placed at (1593.05, 1099.30), ground snap next frame (16 ms)",
                                             "-> FAILED: Enemy look: id 0202B: refused (no ground)"), 3,
     {"TB sticky her": ("INVALID", "did not place him")}, []),
    ("TF1 calls 0", lambda t: sub(t, "shoot: calls 10 redirected 10", "shoot: calls 0 redirected 0"), 3,
     {"TF1 bit 32 on: no round at Sam": ("INCONCLUSIVE", "bit 32 not exercised"), "bit 32 live": ("PASS", None)}, []),
    ("TF1 calls, none redirected", lambda t: sub(t, "shoot: calls 10 redirected 10", "shoot: calls 10 redirected 0"), 1,
     {"TF1 bit 32 on: no round at Sam": ("FAIL", "none redirected")}, []),
    ("no job call reached the hook", no_calls, 1,
     {"bit 32 filter": ("INCONCLUSIVE", "no job call reached the hook"), "bit 32 live": ("FAIL", "never saw a job call")}, []),
    ("every call noslot", lambda t: sub(no_calls(t), "noslot 0", "noslot 4", 0), 1,
     {"bit 32 filter": ("FAIL", "every shoot-job call dropped as noslot")}, []),
    ("noslot over a tenth", lambda t: sub(t, "noslot 0", "noslot 4", 0), 1,
     {"bit 32 filter": ("FAIL", "noslot over a tenth")}, []),
    ("a few noslot", lambda t: sub(t, "noslot 0", "noslot 1", 0), 3,
     {"bit 32 filter": ("INCONCLUSIVE", "a few noslot calls")}, []),
    ("vt_other", lambda t: sub(t, "vt_other 0", "vt_other 1", 0), 1, {"bit 32 filter": ("FAIL", None)}, []),
    ("the type-0x36 branch", lambda t: sub(sub(sub(no_calls(t), "; t36 0", "; t36 7", 0), "shoot: calls 10 redirected 10", "shoot: calls 0 redirected 0"),
                                           "shoot: calls 5 redirected 5", "shoot: calls 0 redirected 0"), 3,
     {"bit 32 live": ("INCONCLUSIVE", "type-0x36"), "bit 32 filter": ("INCONCLUSIVE", "type-0x36"),
      "TF1 bit 32 on: no round at Sam": ("INCONCLUSIVE", "type-0x36"), "TB phase 0: bit 32": ("INCONCLUSIVE", "type-0x36")}, []),
    ("older lines (no t36)", lambda t: resub(t, r"; t36 \d+", ""), 0, {"bit 32 live": ("PASS", None)}, ["gate: PASS"]),
]


def run_case(tmp, name, mutate):
    d = os.path.join(tmp, re.sub(r"\W+", "_", name))
    for src, dst in (("host.log.txt", HOST), ("guest.log.txt", GUEST)):
        os.makedirs(os.path.dirname(os.path.join(d, dst)), exist_ok=True)
        with open(os.path.join(FIX, src), encoding="utf-8") as f:
            text = f.read()
        if mutate and dst == HOST:
            text = mutate(text)
        with open(os.path.join(d, dst), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.run([sys.executable, "-I", "-B", GATE, d], capture_output=True, text=True, encoding="utf-8", env=env)
    return r.returncode, r.stdout + r.stderr


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, mutate, code, want, frags in CASES:
            rc, out = run_case(tmp, name, mutate)
            items = {}
            for line in out.splitlines():
                m = ITEM.match(line)
                if m:
                    items[m.group(1).strip()] = (m.group(2), m.group(3))
            bad = []
            if rc != code:
                bad.append("exit %d, want %d" % (rc, code))
            for item, (st, frag) in want.items():
                got = items.get(item)
                if not got:
                    bad.append("%s: no item line" % item)
                elif got[0] != st or (frag and frag not in got[1]):
                    bad.append("%s: %s (%s), want %s%s" % (item, got[0], got[1], st, (" with '%s'" % frag) if frag else ""))
            for frag in frags:
                if frag not in out:
                    bad.append("no '%s' in the output" % frag)
            if bad:
                fails += 1
                print("FAIL %s: %s" % (name, "; ".join(bad)))
                if "Traceback" in out:
                    print(out[out.index("Traceback"):])
            else:
                print("ok   %s" % name)
    print("%d of %d case(s) failed" % (fails, len(CASES)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
