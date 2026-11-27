"""Regression test of tools/testenv/enemy-target-gate.py (round 27 run 3 review): the gate's verdict rules on a trimmed run-3-format
fixture (tests/fixtures/enemy-target-gate/host.log.txt and guest.log.txt, laid out as a run folder: every item PASS; make_base.py
writes them; *.log is ignored by git) and on mutations of the host log (and, for a case with a sixth field, the guest log), each asserting the gate's exit code and the status (and a
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
ITEM = re.compile(r"^(.+?)\s{2,}(PASS|FAIL|INVALID|INCONCLUSIVE|INFO)\s{2,}(.*)$")


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


PLACE = ("[enemy] tgt place #%d id=0202B (TF0, %s): sent 22.0 m +0 deg (candidate 0), lead +3 deg (rate +9 deg/s) 0.10 m, to (1601.20, "
         "1104.80); her before horiz 31.0 m bearing 40 dz 0.20 pitch 0.4; ground dz - (probe off), chosen -1 of 10, skipped 0 raised")


# the guest's three PLACE line kinds (enemy_combat.cpp GuestPlace): placed, refused by the teleport, refused before it
GUEST_PLACES = ["[enemy] place #4 id=0202B (arm TF0): snap: placed at (1601.20, 1104.80), ground snap next frame",
                "[enemy] place #5 id=0202B (arm TF0): refused: too soon after the last teleport (max 2 per second)",
                "[enemy] place #6 id=0202B (arm TF0) refused (under 700 ms after the last)"]


def no_calls(t):
    return resub(t, NO_CALLS, "shoot 32: calls 0 redirected 0 points 0 (at her 0 Sam 0")


# (M3f) the TB watch before the Sam step (t=1.00) with another sight suffix; the lines an older build does not write
TB_W0 = r"(L1 watch id=0202B t=1\.00:.*); sight bucket 0\.2 s los clear 0\.1 s"
M3F_LINE = re.compile(r"\[enemy\] tgt (?:id=\w+ sight: |los |pick (?:16|32) |arm \w+: her (?:not )?seen )")


def tb_sight(bucket, los, age):
    return lambda t: resub(t, TB_W0, r"\1; sight bucket %s s los %s %s s" % (bucket, los, age))


def older_m3f(t):
    t = resub(t, r"; sight bucket \S+ s los \w+ \S+ s", "")
    t = resub(t, r"; sight keeps \d+ synth \d+ skip \d+ rays \d+; onhit_odd \d+", "")
    t = resub(t, r"; ground dz \S+ \(probe [^)]*\), chosen -?\d+ of \d+, skipped \d+ raised", "")
    t = sub(t, "enemy_target_pick=55", "enemy_target_pick=7")
    return "\n".join(line for line in t.split("\n") if not M3F_LINE.search(line))


# (name, host mutation, expected exit code, {item: (status, reason fragment or None)}, output fragments[, guest mutation])
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
    # run 4: op 11 places her (CL, TF0, TF1, TB): its 6000 ms limit and "; k places" parse and print (op 11 is printed, not gated)
    ("op 11 placed, not valid in 6000 ms",
     lambda t: insert_after(sub(t, "TF0 id=0202B: her valid after 350 ms (horiz 20.0 m, bearing 3); Sam not moved",
                                "TF0 id=0202B: her not valid in 6000 ms (horiz 22.3 m, bearing 104; 5 places); started anyway; Sam placed 170.0 m"),
                            "[enemy] tgt arm-at-geometry TF0 id=0202B", [PLACE % (4, "op 11"), PLACE % (5, "op 11"), PLACE % (6, "kept in view"),
                                                                       "[enemy] tgt place id=0202B (TF0, op 11): not sent (the link did not take it)",
                                                                       "[enemy] tgt arm TF0 id=0202B: kept in view: 1 re-places (it picked her after 900 ms)"]), 0,
     {"TF0 bit 32 off: the actor branch at Sam": ("PASS", None)},
     ["op 11: TF0 id=0202B: her not valid in 6000 ms (horiz 22.3 m, bearing 104; 5 places); started anyway",
      "op 11 PLACEs (run 4): sent 2 at the start, 1 kept in view, 1 not sent (the link did not take it); kept in view 1 re-places "
      "(it picked her after 900 ms); the guest placed 1 refused 2", "PLACE ground probe (M3f): chose -1 of 10, skipped 0 raised, ground dz - "
      "(probe off)", "gate: PASS"],
     lambda t: insert_after(t, "[enemy] spv1 sent", GUEST_PLACES)),
    ("op 11 placed, valid", lambda t: sub(t, "TB id=0202B: her valid after 350 ms (horiz 20.0 m, bearing 3); Sam not moved",
                                          "TB id=0202B: her valid after 1250 ms (horiz 20.1 m, bearing 4; 2 places); Sam not moved"), 0,
     {"TB sticky her": ("PASS", None)}, ["op 11: TB id=0202B: her valid after 1250 ms (horiz 20.1 m, bearing 4; 2 places); Sam not moved"]),
    # (M3f) TB phase 0's precondition: her seen (her bucket record <= 1.5 s old, or the line of sight clear <= 0.5 s) when Sam steps out
    ("TB phase 0: her not seen", tb_sight("9.7", "blocked", "0.1"), 3,
     {"TB phase 0: both visible": ("INVALID", "her not seen by it when our Sam stepped out"),
      "TB sticky her": ("INVALID", "her bucket record 9.7 s old, los blocked 0.1 s"), "TB phase 0: no round at Sam": ("INVALID", None),
      "TB phase 0: bit 32": ("INVALID", None), "TB switch to Sam <= 3 s": ("PASS", None)}, []),
    ("TB phase 0: seen by the line of sight", tb_sight("9.7", "clear", "0.3"), 0,
     {"TB phase 0: both visible": ("PASS", "los clear 0.3 s"), "TB sticky her": ("PASS", None)}, ["gate: PASS"]),
    ("TB phase 0: los clear but stale", tb_sight("-", "clear", "0.9"), 3,
     {"TB phase 0: both visible": ("INVALID", "her bucket record - s old, los clear 0.9 s")}, []),
    # the synth wrote her record and the shoot job never took its actor branch (M3f review: graded on where the shots went): the
    # place branch aiming at her with no round at Sam is the synth's success; a point or a round at Sam FAILs
    ("TB synth, place branch at her", lambda t: sub(sub(t, "shoot: calls 5 redirected 5", "shoot: calls 0 redirected 0"),
                                                    "keeps 0 synth 0 skip 0; her bucket last 3.4 s", "keeps 30 synth 120 skip 0; her bucket last 3.4 s"), 0,
     {"TB phase 0: bit 32": ("PASS", "synth kept her (place branch only, no actor call to redirect): synth 120"),
      "TB sight": ("INFO", "keeps 30 synth 120")}, ["gate: PASS"]),
    ("TB synth, shot at Sam", lambda t: sub(sub(sub(t, "shoot: calls 5 redirected 5 points 9 (at her 9 Sam 0 none 0); last her",
                                                    "shoot: calls 0 redirected 0 points 9 (at her 0 Sam 9 none 0); last Sam"),
                                                "keeps 0 synth 0 skip 0; her bucket last 3.4 s", "keeps 30 synth 120 skip 0; her bucket last 3.4 s"),
                                            "fired_p=4/5/6 at_her_p=4/0/0 at_sam_p=0/5/6", "fired_p=4/5/6 at_her_p=2/0/0 at_sam_p=2/5/6"), 1,
     {"TB phase 0: bit 32": ("FAIL", "synth wrote her record (120) but the shot went to Sam (no actor call to redirect)"),
      "TB phase 0: no round at Sam": ("FAIL", None)}, []),
    # the switch note carries the sight fields
    ("TB switch, the line of sight never kept her",
     lambda t: sub(insert_after(t, "[fixture] tb before op 7", [watch(10, 15.0, 1.2) + "; sight bucket 1.6 s los blocked 0.1 s", SWITCH]),
                   "keeps 0 synth 0 skip 0; her bucket last 3.4 s", "keeps 0 synth 0 skip 0; her bucket last 2.0 s"), 1,
     {"TB sticky her": ("FAIL", "sight at that watch: bucket 1.6 s, los blocked 0.1 s; phase 0 sight: los clear 12 blocked 0 out 0 low_vis 0, "
                                "keeps 0 synth 0 skip 0 (the line of sight never kept her")}, []),
    # an older build: no sight suffix, no SIGHT / pick-bit / self-test / seen lines, no summary tail, no PLACE probe: parses
    ("older lines (no M3f)", older_m3f, 3,
     {"TB phase 0: both visible": ("INCONCLUSIVE", "an older build"), "TB sticky her": ("PASS", None), "TB phase 0: bit 32": ("PASS", None),
      "TB sight": ("INFO", "self-test none; bits no pick 16 / 32 line"), "CL level: notice <= 2 s": ("PASS", None)},
     ["gate: INCOMPLETE (TB phase 0: both visible)"]),
    # the key has bit 16 and the raycast's self-test failed: bit 16 never came on
    ("TB sight: self-test failed", lambda t: sub(t, "[enemy] tgt los self-test: ok (hit 1.21 m below the chest)",
                                                 "[enemy] tgt los self-test: failed (0, hit z 0.00 vs root 101.30) -- the raycast misses the ground; "
                                                 "pick 16 off"), 1,
     {"TB sight": ("FAIL", "the raycast's self-test failed")}, ["gate: FAIL (TB sight)"]),
    ("TB sight: no self-test", lambda t: "\n".join(line for line in t.split("\n") if "los self-test" not in line), 1,
     {"TB sight": ("FAIL", "self-test missing")}, []),
    # onhit_odd: information only, with its once-line
    ("onhit_odd", lambda t: insert_after(sub(t, "onhit_odd 0", "onhit_odd 2", 0), "[enemy] tgt redirect 32 (shoot actor)",
                                         ["[enemy] tgt onhit odd first: att 0x7FF6A0001000 vt 0x7FF6A1234560 (no RTTI name) inst 0x7FF6A0002000 "
                                          "inst_vt 0x0"]), 0,
     {"onhit_odd host": ("INFO", "2; first: onhit odd first: att 0x7FF6A0001000"), "soft host": ("PASS", None)},
     ["host once onhit_odd (", "gate: PASS"]),
    ("CL PLACE ground probe", None, 0, {"CL level: notice <= 2 s": ("PASS", "last PLACE chose 6 of 10, skipped 3 raised, ground dz 0.07 "
                                                                            "(probe hit, los clear)")},
     ["PLACE ground probe (M3f): chose 6 of 10, skipped 3 raised, ground dz 0.07 (probe hit, los clear)"]),
]


def run_case(tmp, name, mutate, gmutate=None):
    d = os.path.join(tmp, re.sub(r"\W+", "_", name))
    for src, dst in (("host.log.txt", HOST), ("guest.log.txt", GUEST)):
        os.makedirs(os.path.dirname(os.path.join(d, dst)), exist_ok=True)
        with open(os.path.join(FIX, src), encoding="utf-8") as f:
            text = f.read()
        if mutate and dst == HOST:
            text = mutate(text)
        if gmutate and dst == GUEST:
            text = gmutate(text)
        with open(os.path.join(d, dst), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    r = subprocess.run([sys.executable, "-I", "-B", GATE, d], capture_output=True, text=True, encoding="utf-8", env=env)
    return r.returncode, r.stdout + r.stderr


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        for case in CASES:
            name, mutate, code, want, frags = case[:5]
            rc, out = run_case(tmp, name, mutate, case[5] if len(case) > 5 else None)
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
