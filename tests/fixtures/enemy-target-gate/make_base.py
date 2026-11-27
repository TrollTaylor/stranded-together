"""Write the enemy-target-gate base fixture (a trimmed run-3-format host / guest log in which every gate item PASSes); synthetic, not
evidence. tests/enemy_target_gate_test.py lays them out as a run folder (game/ and guest/game/stranded_together.log) and mutates
the host log per case. (*.log is ignored by git: the fixture is host.log.txt / guest.log.txt.) Round 27 M4: the destroy guard line,
the self-test's three-layer format and the guard summary suffix; tests/enemy_target_gate_test.py builds the defaults-mode run from
it (DEFAULTS).

    python -I tests/fixtures/enemy-target-gate/make_base.py tests/fixtures/enemy-target-gate
"""
import os
import sys

root = sys.argv[1]
t = [5.0]
host, guest = [], []


def H(text, dt=0.25):
    t[0] += dt
    host.append("[%9.3f][94712] %s" % (t[0], text))


def G(text, dt=0.25):
    guest.append("[%9.3f][36368] %s" % (t[0] + dt, text))


SUM = ("[enemy] tgt%s: engaging her %d, Sam 0; reorders 4; suppress 2; redirects 1:3 2a:5 (offset 0) 2b:0 4:1 (not ours 0) veh:0; h5 0 h2 0/0 "
       "(no type-1 bucket 0) forced 0; registered 15; faults 0; mismatch 0; table clears 0; shoot 32: calls %d redirected %d points %d (at her %d Sam %d "
       "none 0); soft onhit_bad 0 psr_rec_bad 0 turn_goal_bad 0; shoot32 vt_other 0 noslot 0; t36 0; sight keeps 0 synth 0 skip 0 rays 120; "
       "onhit_odd 0; guard calls 120 (4/s) matched 0 dead-refused 0 trips 0; take-first 0; cls 900/s psr 300/s")
CHECKS = ("[enemy] tgt byte checks (71): stimulus ok, registry ok, goals ok, sensor results ok, classifier ok, classifier flags ok, forced visual ok, "
          "MULE members ok, player objects ok, redirect 1 ok, 2a ok, 2b ok, 4 ok, 16 (vehicle chase) ok, 32 (shoot actor) ok; redirect 8 deferred (not built)")
SEH_T = "[seh] test: a guarded read of a reserved page faulted and was caught; absorbed 0 -> 2, reported 0 -> 0 (guard on)"
SEH_A = ("[seh] absorbed 2 fault(s) inside the mod's guards (last: code 0xC0000005 at mod rva 0x1F8CAC, from mod rva 0x0, mod rva 0x0); passed to the "
         "game's report 0, walk faults 0")
GEO = ("[enemy] tgt geometry id=%s (%s): her dz %.2f m horiz %.1f m pitch 1.0 bearing %d valid %d; Sam dz 0.50 m horiz %.1f m bearing %d pitch 1.0; "
       "rule %s (%s); sep %d")
WATCH = ("[probe] L1 watch id=%s t=%.2f: phase 5 ea4 0x42440000; faces her %d deg / Sam %d deg; her %.1f m Sam %.1f m; PV 1 at %s 0.0 m id 00000 "
         "age %.1f; LK 1 at her; goal 0 id 0x0 mode 1; shoot pt -> her (calls 0 redir 0); rounds 0; sight bucket 0.2 s los clear 0.1 s")
SHOOT = ("[enemy] tgt id=%s shoot: calls %d redirected %d points %d (at her %d Sam %d none 0); last %s mode %d age 0.1 s, her 15.0 m, Sam 15.0 m; "
         "modes actor %d place %d other 0; t36 0")
BASE_RES = ("stim_start=1 stim_n=60 stim_lost=0 releases=0 pv_start=0 calm_timeout=0 goals=1 valid=1 rays=%d tie=her calm=ready samples=60 "
            "noticed=%d turned_her=%d turned_sam=%d approached_her=0 approached_sam=0 aim_n=5 aim_her=5 aim_sam=0 fired=%d at_her=%d at_sam=%d "
            "pv_her=60 pv_sam=%d switch_sam=%d switch_her=-1 cls_her=%d cls_sam=10 app_her=10 app_sam=0 rec0_her=10 rec0_sam=0 fired_p=%s "
            "at_her_p=%s at_sam_p=%s")
# (M3f) the SIGHT line (op 7 and op 8, after the shoot line)
SIGHT = ("[enemy] tgt id=%s sight: los clear %d blocked 0 out 0 low_vis 0; keeps %d synth %d skip 0; her bucket last %s s (gap lead 120 tail 80 ms)")
NO_SHOOT = ("calls 0 redirected 0 points 0 (at her 0 Sam 0 none 0); last - mode -1 age -1.0 s, her -1.0 m, Sam -1.0 m; "
            "modes actor 0 place 0 other 0; t36 0")


def arm(label, mid, geo, vis=1.00, stim="her", armgeo="her valid after 350 ms (horiz 20.0 m, bearing 3); Sam not moved", body=None, res=None,
        shoot=None, engaged=False, seen=None, place=None):
    if place:
        H(place)
    if seen:
        H("[enemy] tgt arm %s: her %s" % (label, seen))
    H("[enemy] tgt arm-at-geometry %s id=%s: %s" % (label, mid, armgeo))
    H("[probe] L1 arm %s id=%s: start (stim %s, cp 04004)" % (label, mid, stim))
    H("[enemy] tgt arm %s id=%s: start (stim %s, vis %.2f, aim 1.17 m SPV1, pick live 7, redirect sites 0x37, pick reset)" % (label, mid, stim, vis), 0)
    H(geo, 0)
    if engaged:
        H("[enemy] tgt id=%s -> her (only visible)" % mid)
    if body:
        body()
    H("[enemy] tgt arm %s id=%s end: pick her; flips her->Sam 0; her bucket gap max 300 ms" % (label, mid))
    H("[enemy] tgt id=%s shoot: %s" % (mid, shoot or NO_SHOOT), 0)
    H(SIGHT % (mid, 30, 0, 0, "4.0"), 0)
    H(SUM % (" arm end", 1, 0, 0, 0, 0, 0), 0)
    H("[probe] L1 result arm %s id=%s: %s" % (label, mid, res), 0.05)


H("[config] enemy_partner_stim=1 enemy_partner_vis=1 enemy_partner_stim_radius=300 enemy_target_pick=55 enemy_target_redirect=39 "
  "enemy_target_fresh_ms=1500")
H(CHECKS)
G(CHECKS)
H("[enemy] tgt destroy guard: RemoveEntityNow hook on (byte checks 6/6)")
H("[enemy] tgt pick 16 (line of sight): off (waiting for the raycast's self-test)")
H("[enemy] tgt pick 32 (her record from ours): off (bit 16 not live)", 0)
H("[enemy] tgt los self-test: L32 hits 1 z 101.30, L47 hits 1 z 101.30, L52 hits 0 (control) vs root 101.30; attempt 1 -- ok")
H("[enemy] tgt pick 16 (line of sight): on (the self-test passed, bit 1 live)")
H("[enemy] tgt pick 32 (her record from ours): on (bit 16 live)", 0)
H("[enemy] tgt redirect 32 (shoot actor): on (bit 32 on)")
H("[enemy] pstim made #1 (id 0x0, stim 0000068247DC2620, agent 000006824ACD5F90; nearest enemy 60 m, radius 300)")
G("[enemy] pstim not made (the guest (host only))")
H("[enemy] pstim cycles 50: made 50 removed 50 faults 0; made again #52")
H(SEH_T)
G(SEH_T)
H(SEH_A)
G(SEH_A)
H("[probe] L1 byte checks (36): stimulus ok, classifier ok (shared), append ok, sensor results ok, goals ok, daemon ok, phase ok, melee ok, reads ok, "
  "registry ok")
H("[probe] L1 calm #1 id=0202B: phase 0 on 9 members, the CP at rest 2")
H("[probe] L1 calm #1 id=0202B: ready (3.1 s; phase 0; PV 0 LK 1; 2 applies); members at 3-5 0/9, cp 2; CP forces 0, PV age -1.0")
# TA
arm("TA", "0202B", GEO % ("0202B", "TA", 0.6, 23.3, 1, 1, 150.7, 178, "gun", "her", 178),
    res=BASE_RES % (5, 535, 535, -1, 5, 5, 0, 0, -1, 1200, "0/0/0", "0/0/0", "0/0/0"))


def cl_body():  # the stimulus made 141 ms after the watch start
    H("[enemy] pstim made #2 (id 0x0, stim 0000068247DC2620, agent 000006824ACD5F90; nearest enemy 4 m, radius 300)", 0.141)


arm("CL", "02029", GEO % ("02029", "CL", 0.07, 4.2, 18, 1, 152.0, 166, "melee", "her", 166), body=cl_body,
    place=("[enemy] tgt place #2 id=02029 (CL, op 11): sent 4.0 m -35 deg (candidate 6), lead +0 deg (rate +0 deg/s) 0.00 m, to (1588.10, "
           "1097.40); her before horiz 150.3 m bearing 120 dz 0.40 pitch 0.1; ground dz 0.07 (probe hit, los clear), chosen 6 of 10, "
           "skipped 3 raised"),
    res=BASE_RES % (0, 900, 900, -1, 0, 0, 0, 0, -1, 900, "0/0/0", "0/0/0", "0/0/0"))
# TE (crouched: vis 0.60 below TA's 1.00)
arm("TE", "0202C", GEO % ("0202C", "TE", 0.1, 16.0, 5, 1, 150.0, 170, "gun", "her", 170), vis=0.60,
    res=BASE_RES % (0, 1900, 1900, -1, 0, 0, 0, 0, -1, 1100, "0/0/0", "0/0/0", "0/0/0"))
# TF0 (bit 32 off: the actor branch resolves to Sam)
H("[enemy] tgt redirect bits for this run: 7 (live sites 0x17)")
arm("TF0", "0202B", GEO % ("0202B", "TF0", 0.5, 22.0, 2, 1, 170.0, 40, "gun", "her", 40), engaged=True,
    shoot=SHOOT % ("0202B", 6, 0, 12, 0, 12, "Sam", 0, 12, 0),
    res=BASE_RES % (6, -1, -1, -1, 6, 0, 6, 0, -1, 1000, "0/0/0", "0/0/0", "0/0/0"))
# TF1 (bit 32 on: redirected, the point at her)
H("[enemy] tgt redirect bits for this run: 39 (the key) (live sites 0x37)")


def tf1_body():
    H("[fixture] tf1 summary")
    H(SUM % ("", 1, 10, 10, 20, 20, 0))


arm("TF1", "0202B", GEO % ("0202B", "TF1", 0.5, 22.0, 2, 1, 170.0, 40, "gun", "her", 40), engaged=True, body=tf1_body,
    shoot=SHOOT % ("0202B", 10, 10, 20, 20, 0, "her", 1, 0, 20),
    res=BASE_RES % (9, -1, -1, -1, 9, 9, 0, 0, -1, 1000, "0/0/0", "0/0/0", "0/0/0"))


def tb_body():  # her first; the Sam step; phase 0 sticky her (op 7); she hides (phase 1: -> Sam); she is back (phase 2: stays Sam)
    H(WATCH % ("0202B", 1.0, 2, 178, 15.0, 150.0, "her", 0.0))
    H("[enemy] look id=0202B dist 15.0 m (25 deg off its line to her): snap: placed at (1593.05, 1099.30), ground snap next frame")
    H("[auto] 0065:1 action enemy_look -11 15 25 -> ok: Enemy look: id 0202B: snap: placed at (1593.05, 1099.30), ground snap next frame (16 ms)", 0)
    H("[fixture] tb phase 0")
    H("[enemy] tgt los id=0202B clear (hits 0; 15.0 m; hit (0.00, 0.00, 0.00))")
    for k in range(4):
        H(WATCH % ("0202B", 2.0 + k * 0.25, 3, 22, 15.0, 15.0, "her", 0.0))
    H(SUM % ("", 1, 5, 5, 9, 9, 0))
    H("[fixture] tb before op 7")
    H(GEO % ("0202B", "op 7", 0.4, 15.0, 3, 1, 15.0, 22, "gun", "her", 25))
    H("[enemy] tgt id=0202B pick her; flips her->Sam 0; her bucket gap max 120 ms", 0)
    H(SHOOT % ("0202B", 5, 5, 9, 9, 0, "her", 1, 0, 9), 0)
    H(SIGHT % ("0202B", 12, 0, 0, "3.4"), 0)
    H("[enemy] tgt vis forced 0.00")
    H("[enemy] tgt id=0202B -> Sam (only visible)", 1.2)
    H("[enemy] tgt vis forced 1.00", 4.8)


arm("TB", "0202B", GEO % ("0202B", "TB", 0.4, 15.0, 1, 1, 150.0, 178, "gun", "her", 178), engaged=True, body=tb_body,
    seen="seen (bucket 0.2 s, los clear) after 400 ms",
    shoot=SHOOT % ("0202B", 2, 0, 15, 0, 15, "Sam", 0, 15, 0),
    res=BASE_RES % (15, -1, -1, 1200, 15, 4, 11, 20, 1200, 1000, "4/5/6", "4/0/0", "0/5/6"))
# CH, CS, TX: printed, not gated
arm("CH", "02029", GEO % ("02029", "CH", 0.1, 5.0, 4, 1, 150.0, 170, "melee", "her", 170),
    res=BASE_RES % (0, 535, 535, -1, 0, 0, 0, 0, -1, 386, "0/0/0", "0/0/0", "0/0/0"))
arm("CS", "02029", "[enemy] tgt geometry id=02029 (CS): her dz 0.54 m horiz 147.1 m pitch 0.0 bearing 64 valid 1; Sam dz -0.67 m horiz 8.0 m "
    "bearing 12 pitch 1.0; rule melee (Sam); sep 52", stim="none", armgeo="her not judged (a Sam arm); Sam placed 8.0 m in front",
    res=BASE_RES % (0, -1, -1, 2088, 0, 0, 0, 1897, -1, 0, "0/0/0", "0/0/0", "0/0/0"))
H("[enemy] tgt redirect bits for this run: 0 (live sites 0x0)")
arm("TX", "0202C", GEO % ("0202C", "TX", -0.6, 14.0, 1, 1, 152.6, 178, "gun", "her", 178),
    res=BASE_RES % (0, 540, 540, -1, 0, 0, 0, 0, -1, 900, "0/0/0", "0/0/0", "0/0/0"))
H(SUM % (" status", 0, 0, 0, 0, 0, 0))
H("[enemy] spv1 sent 2632 recv 2548 stale 0 bad 0; aim ours 1.21 m, hers 1.30 m (default); our down 0")
G("[enemy] spv1 sent 2532 recv 2580 stale 0 bad 0; aim ours 1.15 m, hers 1.21 m (SPV1); our down 0")
for path, ls in ((os.path.join(root, "host.log.txt"), host), (os.path.join(root, "guest.log.txt"), guest)):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(ls) + "\n")
print(len(host), len(guest))
