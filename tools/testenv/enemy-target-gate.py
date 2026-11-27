"""Round 27 run 3 / 4 gate (docs/enemy-sync.md 16.6): the arm table and PASS / FAIL / INVALID per gate item (two-games-enemy-target).

    python -I tools/testenv/enemy-target-gate.py <run folder>

Reads <run>/game/stranded_together.log (the host) and <run>/guest/game/stranded_together.log (the guest). The arms are the L1
probe's watch ("[probe] L1 result arm X id=XXXXX: key=value ..."; enemy-target.py reads the M1 arms of the same lines), the rest
the "[enemy] pstim / spv1 / tgt ..." and "[seh] ..." lines. Gate items:
  checks    both games: "[enemy] tgt byte checks (n): ..." with no FAILED part (redirect 8 is "deferred")
  made      the host: "[enemy] pstim made #n" at least once
  host only the guest: no "pstim made" line and a "pstim not made (the guest (host only))" line
  cycles    the host: "[enemy] pstim cycles 50: made 50 removed 50 faults 0; made again #n" (U15)
  spv1      both games: the last "[enemy] spv1 sent n recv m" with n > 0 and m > 0
  faults    no "[enemy] ... FAULT" / "tgt byte check FAILED" line in either game
  seh test  both games: "[seh] test: a guarded read of a reserved page faulted and was caught" (enemy_tgt 10)
  seh       both games: the last "[seh] absorbed N fault(s)" with N <= 2 (op 10's self-test absorbs two: a noexcept guard and
            one that is not); over 2 FAILs and prints the first line over 2 (its time, the fault's rva and its two callers)
  bit 32 filter  the host's summary totals "shoot32 vt_other n noslot m" (and the calls): vt_other > 0 FAILs (a carrier class
            the hook did not anticipate); noslot > 0 with no call counted, or noslot over max(3, calls / 10): FAIL (a MULE whose
            calls are never redirected); no call at all (the counters live only in the job's id-0 call branch): INCONCLUSIVE (the
            type-0x36 branch named when "t36" > 0); a few noslot calls: INCONCLUSIVE (the once-line "tgt shoot 32: carrier ... is
            not a registered MULE" printed)
  bit 32 live  the job's actor-branch calls over TF1 and TB (the arms' 5 s summaries): calls > 0 and redirected > 0 PASS; calls
            0 with "t36" > 0 (the MULE wrapper's type-0x36 branch, which never calls SetTargetActor): INCONCLUSIVE; else FAIL
  TA        noticed and turned to her, no round at Sam; when it fired (>= 1), >= 60 % of the rounds within 1.5 m of her chest
  TB        sticky: a "tgt id=0202B -> her" line before the Sam step (enemy_look -11 15 25, older runs -10 15), no "-> Sam"
            between the arm's start and its op 7 line, and 0 her->Sam flips on op 7's line (the unlogged ones too). Any logged
            "-> Sam" there FAILs, whatever its rule (with her current only "hit by" or "only visible" can switch; our Sam never
            fires): the last watch line before it explains it (faces her > 60 deg: it turned off her, the turn redirect did not
            hold, with her PV age at the first watch past 60 deg; else her record left its PlayerVisual bucket while it faced
            her). INVALID only for causes the scenario made: the Sam step's line without "placed", her not 10-30 m from it at
            the last watch before the switch, or (no switch, no flip) Sam not in its cone on op 7's geometry line (horiz 10-20
            m, bearing <= 30, sep 15-35) or her not 10-30 m there. Phase 0 (her visible, then Sam too): no round at Sam
            (fired_p[0] 0: INCONCLUSIVE). Phase 0 bit 32 (the arm MULE's shoot line right after op 7): no actor-branch call:
            INCONCLUSIVE (the type-0x36 branch named when its t36 > 0);
            else PASS when redirected > 0, the point never at Sam and no round at Sam in phase 0. The switch to Sam <= 3 s after
            she hides (switch_sam -2, phase 0 ended on Sam: INCONCLUSIVE). Phase 2 (she is back, Sam still in view): stays Sam:
            no "-> her" after "tgt vis forced 1.00" and no round at her (INCONCLUSIVE when phase 1 never switched (-1), or
            phase 0 ended on Sam (-2) and phase 1 did not fire at Sam only)
  TF0       (bit 32 off, her 22 m in front along its facing, Sam 140-210 m away, 30-50 deg off its line to her ("sep"), beyond
            sight) the shoot line's per-mode counts: no actor-branch point (modes actor 0): "place branch only", no item; the
            actor branch taken and nothing redirected: PASS when the points or the rounds reached Sam, else FAIL; anything
            redirected FAILs (bit 32 was not off)
  TF1       (the same placement made again, bit 32 on) FAIL on a round at Sam or the shoot point at Sam (Sam n > 0 or last
            Sam); fewer than 3 rounds: INCONCLUSIVE; no actor-branch call (calls 0: bit 32 not exercised; TF0 with bit 32 off
            fires at her too): INCONCLUSIVE; calls > 0 with nothing redirected FAILs; PASS: calls > 0, redirected > 0, the point
            at her and >= 2/3 of the rounds at her
  TE        her crouched: the arm-start vis ("[enemy] tgt arm TE ...: start (..., vis v, ...") <= 0.7 and lower than TA's, and
            the classifier saw her (cls_her > 0); the notice timing (TE > TA + 1 s, or none) is printed, not gated
  CL        (her along its facing) noticed <= 2 s after her stimulus was made (the notice time less the arm start -> the first
            "pstim made" line after it) with |dz| <= 1.0 on its geometry line: PASS; a stimulus over 1 s late, or a notice before
            it: INCONCLUSIVE; not noticed with her bearing > 45 deg off its facing (outside the melee cone): INCONCLUSIVE;
            otherwise (a missed or late notice in the cone, or |dz| > 1) FAIL
  soft      both games: the soft counters (onhit_bad, psr_rec_bad, turn_goal_bad; their totals) all 0, else INCONCLUSIVE (the
            host's "tgt onhit_bad first" once-line is printed with it); M3f: "onhit_odd" (an attacker of the game's whose +0x338
            is not the game's) is an INFO item with its "tgt onhit odd first" once-line
  M3f       (pick bits 16, the line-of-sight keep, and 32, her record from ours; docs/enemy-sync.md 16.2 / 16.6) "TB phase 0:
            both visible": the last watch line before the Sam step has her bucket record <= 1.5 s old or the line of sight clear
            <= 0.5 s old ("; sight bucket b s los v a s"), else INVALID with TB sticky and the other phase-0 items (no suffix:
            INCONCLUSIVE); TB sticky's switch note adds the sight fields; "TB phase 0: bit 32" FAILs when the SIGHT line after
            op 7 counts synth > 0 and the shoot line no actor-branch call; "TB sight" (INFO) prints the raycast's self-test, the
            "tgt pick 16 / 32" transitions and the SIGHT numbers, and FAILs only when the key (the "[config]" line, else
            session-host-deployed.ini) has bit 16, TB counts and the self-test is missing or failed; CL prints the PLACE's
            ground probe (the chosen candidate, the raised ones skipped). INFO items never change the exit code.
  rays      TA, TB, TF0, TF1 (the arms whose verdict counts rounds): every round attributed by its ray (rays == fired), else
            INCONCLUSIVE (the rest fell back to the sample line)
An arm says nothing (its items INVALID, not FAIL) when: its own geometry line (logged at its start, by that arm's rule) says
valid 0; for the engaged arms (TB, TF0, TF1) no "tgt id=<id> -> her" line after its start (TB: before the Sam step) or her
stimulus lost during it (their calm and pv_start do not count; TF0 / TF1 also need Sam 140-210 m away and 30-50 deg off its
line to her (the geometry line's "sep"; older lines: his bearing off its facing)); for TA, CL and TE (CALM_NEEDED: the notice
needs a calm MULE) a re-calm that timed out (calm_timeout=1) or a watch that was not valid; for CH and TX her stimulus lost
during the arm (their calm need not be ready); CS has no stimulus. TX (the redirects off), CH (her level 5 m in front of
02029) and CS (Sam in front, no stimulus) are printed, not gated. Printed too: op 11's "tgt arm-at-geometry" line per arm (run
4: for CL, TF0, TF1 and TB op 11 places her by SCB1 PLACEs, 6 s at most, then keeps CL / TF0 / TF1 in view; the per-arm PLACE
counts, the "kept in view" line and the guest's placed / refused counts are printed, not gated),
the calm end lines, the probe's byte-check line and the once-lines (shoot 32, onhit_bad first, mismatch). The redirect /
reorder totals are the sums of the 5 s "[enemy] tgt: ..." lines; per arm, the sums of those within the arm (every MULE) and the
arm MULE's own bit-32 numbers (its shoot line at the arm's end). Exit 0 all PASS; 1 any FAIL or an arm line missing; 3 no FAIL
but INVALID / INCONCLUSIVE items. Old logs (without the run-2 / run-3 lines) parse: their new items are INVALID or
INCONCLUSIVE. Read only; no game.
"""
import os
import re
import sys

TS = re.compile(r"^\[\s*([0-9.]+)\]")
RESULT = re.compile(r"\[probe\] L1 result arm (\w+) id=([0-9A-F]{5}): (.*)$")
START = re.compile(r"\[probe\] L1 arm (\w+) id=([0-9A-F]{5}): start")
TSTART = re.compile(r"\[enemy\] tgt arm (\w+) id=([0-9A-F]{5}): start \(stim (\w+), vis ([0-9.\-]+), aim ([0-9.\-]+) m (\w+),")
TEND = re.compile(r"\[enemy\] tgt arm (\w+) id=([0-9A-F]{5}) end: pick (\S+); flips her->Sam (\d+); her bucket gap max (\d+) ms")
FLIPS7 = re.compile(r"\[enemy\] tgt id=([0-9A-F]{5}) pick (\S+); flips her->Sam (\d+); her bucket gap max (\d+) ms")
SHOOT = re.compile(r"\[enemy\] tgt id=([0-9A-F]{5}) shoot: calls (\d+) redirected (\d+) points (\d+) \(at her (\d+) Sam (\d+) none (\d+)\); "
                   r"last (\S+) mode (-?\d+) age ([0-9.\-]+) s, her ([0-9.\-]+) m, Sam ([0-9.\-]+) m"
                   r"(?:; modes actor (\d+) place (\d+) other (\d+))?"  # run 3: the points per carrier mode (0 actor, 1 place)
                   r"(?:; t36 (\d+))?")  # run 3 review: the job's calls the MULE wrapper's type-0x36 branch took
CHECKS = re.compile(r"\[enemy\] tgt byte checks \((\d+)\): (.*)$")
FAILED = re.compile(r"\[enemy\] tgt byte check FAILED: (.*)$")
MADE = re.compile(r"\[enemy\] pstim made #(\d+)")
NOTMADE_GUEST = re.compile(r"\[enemy\] pstim not made \(the guest \(host only\)\)")
CYCLES = re.compile(r"\[enemy\] pstim cycles (\d+): made (\d+) removed (\d+) faults (\d+).*; (made again #\d+|not made again \(.*\))$")
SPV = re.compile(r"\[enemy\] spv1 sent (\d+) recv (\d+) stale (\d+) bad (\d+); aim ours ([0-9.\-]+) m, hers ([0-9.\-]+) m \((\w+)\)")
PSTIM = re.compile(r"\[enemy\] pstim: (\w+), vis ([0-9.\-]+)")
FAULT = re.compile(r"\[enemy\] (?:.*FAULT|tgt byte check FAILED)")
# the third group: run 3 prints the rule of the change itself (chg_rule: "only visible", "sticky", "nearer", "hit by", "default
# Sam", ...); older builds the state's rule at the 4 Hz log (a rule-3 switch read "sticky" one tick later)
CHANGE = re.compile(r"\[enemy\] tgt id=([0-9A-F]{5}) -> (\S+) \(([^)]*)\)")
# run 2: Sam's dz / horiz / bearing (and pitch, the rule) after hers; the old line ends at Sam's bearing; run 3: the angle at
# the MULE between her and Sam ("sep", -1 unread)
GEOM = re.compile(r"\[enemy\] tgt geometry id=([0-9A-F]{5}) \(([^)]*)\): her dz ([0-9.\-]+) m horiz ([0-9.\-]+) m pitch ([0-9.\-]+) bearing ([0-9.\-]+) "
                  r"valid (\d)(?:; Sam dz ([0-9.\-]+) m horiz ([0-9.\-]+) m bearing ([0-9.\-]+))?(?: pitch ([0-9.\-]+))?(?:; rule (\w+) \((\w+)\))?"
                  r"(?:; sep ([0-9.\-]+))?")
# run 3: op 11's one line before the arm's start (her valid after n ms / not valid in 2500 ms / not judged (CS)), or cancelled;
# run 4: the placed arms (CL, TF0, TF1, TB) "not valid in 6000 ms" and "...; k places)" in the parentheses
ARMGEO = re.compile(r"\[enemy\] tgt arm-at-geometry (\w+|\?) id=([0-9A-F]{5}): (her valid after (\d+) ms|her not valid in \d+ ms|"
                    r"her not judged \(a Sam arm|cancelled \(([^)]*)\))(.*)$")
# run 4: op 11's PLACEs to the guest (host: sent, per arm; "kept in view" after the start), its end line, and the guest's answer
PLACE_SENT = re.compile(r"\[enemy\] tgt place #\d+ id=[0-9A-F]{5} \((\w+|\?), (op 11|kept in view)\): sent ")
PLACE_NOT = re.compile(r"\[enemy\] tgt place id=[0-9A-F]{5} \((\w+|\?), [^)]*\): not sent \((.*)\)$")
KEPT = re.compile(r"\[enemy\] tgt arm (\w+|\?) id=[0-9A-F]{5}: kept in view: (\d+) re-places \((.*)\)$")
# (": refused: <teleport text>" and " refused (<why>)" refused; ": <teleport text>" placed; ": refused" tried before ": ")
PLACE_RX = re.compile(r"\[enemy\] place #\d+ id=[0-9A-F]{5} \(arm (\w+|\?)\)(: refused| refused|: )")
WATCH = re.compile(r"\[probe\] L1 watch id=([0-9A-F]{5}) t=[0-9.\-]+: .*?faces her (-?\d+) deg")
# the same line's distances and its youngest PlayerVisual entry (who, age): "her 13.0 m Sam 11.8 m; PV 1 at her 0.0 m id 00000 age 1.5"
WATCH_X = re.compile(r"; her ([0-9.\-]+) m Sam ([0-9.\-]+) m; PV (-?\d+) at (\w+) [0-9.\-]+ m id \w+ age ([0-9.\-]+)")
# (M3f) the host's sight suffix: her bucket record's age (s), the line-of-sight verdict and its age (s; "-" none)
WATCH_S = re.compile(r"; sight bucket ([\d.]+|-) s los (\w+) ([\d.]+|-) s")
# (M3f) pick bits 16 / 32: the line-of-sight keep and her record from ours. The SIGHT line (op 7 and op 8, after the shoot line),
# the verdict changes, the bit transitions, the raycast's self-test, op 11's TB "her seen" wait, the config key
SIGHT = re.compile(r"\[enemy\] tgt id=([0-9A-F]{5}) sight: los clear (\d+) blocked (\d+) out (\d+) low_vis (\d+); keeps (\d+) synth (\d+) "
                   r"skip (\d+); her bucket last ([\d.]+|-) s \(gap lead (\d+) tail (\d+) ms\)(?:; (rec \+0x24 .*))?")
LOS = re.compile(r"\[enemy\] tgt los id=([0-9A-F]{5}) her (\w+) \((.*)\)$")
PICKBIT = re.compile(r"\[enemy\] tgt pick (16|32) \([^)]*\): (on|off) \((.*)\)$")
SELFTEST = re.compile(r"\[enemy\] tgt los self-test: (ok|failed) (.*)$")
SEEN = re.compile(r"\[enemy\] tgt arm (\w+): her (seen|not seen) (.*)$")
CONFIG_PICK = re.compile(r"\[config\] .*\benemy_target_pick=(\d+)")
# (M3f) the PLACE line's ground probe: the chosen point's dz, the chosen candidate (-1: none valid, the cycling one) and the raised
PLACE_PROBE = re.compile(r"; ground dz (\S+) \(probe ([^)]*)\), chosen (-?\d+) of (\d+), skipped (\d+) raised")
CALM_END = re.compile(r"\[probe\] L1 calm #(\d+) id=([0-9A-F]{5}): (ready|timeout) \((.*)$")
CALM_APPLY = re.compile(r"\[probe\] L1 calm #(\d+) id=([0-9A-F]{5}): phase 0 on (-?\d+) members, the CP (.*)$")
PROBE_CHECKS = re.compile(r"\[probe\] L1 byte checks \((\d+)\): (.*)$")
# run 3: the once-lines (the first of each)
ONCE = re.compile(r"\[enemy\] tgt (shoot 32: the job's carrier has vtable .*|shoot 32: carrier .* is not a registered MULE|onhit_bad first: .*|"
                  r"onhit odd first: .*|mismatch id=.*)$")
SUMMARY = re.compile(r"\[enemy\] tgt(?: status| arm end)?: engaging her (\d+), Sam (\d+); reorders (\d+); suppress (\d+); redirects 1:(\d+) "
                     r"2a:(\d+) \(offset (\d+)\) 2b:(\d+) 4:(\d+) \(not ours (\d+)\) veh:(\d+); h5 (\d+) h2 (\d+)/(\d+) \(no type-1 bucket (\d+)\) "
                     r"forced (\d+); registered (\d+); faults (\d+); mismatch (\d+); table clears (\d+)"
                     r"(?:; shoot 32: calls (\d+) redirected (\d+) points (\d+) \(at her (\d+) Sam (\d+) none (\d+)\); "
                     r"soft onhit_bad (\d+) psr_rec_bad (\d+) turn_goal_bad (\d+))?"
                     r"(?:; shoot32 vt_other (\d+) noslot (\d+))?"  # run 3: bit 32's carrier filter (totals)
                     r"(?:; t36 (\d+))?"  # run 3 review: the type-0x36 branch (total)
                     r"(?:; sight keeps (\d+) synth (\d+) skip (\d+) rays (\d+); onhit_odd (\d+))?")  # M3f: bits 16 / 32 (totals)
SUM_NAMES = ("her", "sam", "reorders", "suppress", "r1", "r2a", "r2a_offset", "r2b", "r4", "r4_not_ours", "veh", "h5", "h2_refuse", "h2_fix",
             "h2_nobucket", "forced", "registered", "faults", "mismatch", "table_clears", "sh_calls", "sh_redir", "sh_pts", "pt_her", "pt_sam",
             "pt_none", "onhit_bad", "psr_rec_bad", "turn_goal_bad", "vt_other", "noslot", "t36", "keeps", "synth", "synth_skip", "rays",
             "onhit_odd")
SUM_LAST = ("registered", "faults", "mismatch", "table_clears", "onhit_bad", "psr_rec_bad", "turn_goal_bad", "vt_other",
            "noslot", "t36", "keeps", "synth", "synth_skip", "rays", "onhit_odd")  # totals already: the last value
SOFT = ("onhit_bad", "psr_rec_bad", "turn_goal_bad")
REDIR = re.compile(r"\[enemy\] tgt redirect (\S+) \(([^)]*)\): (.*)$")
SAM_STEP = re.compile(r"action enemy_look -1[01] 15\b")  # run 3: -11 15 25 (off its line to her); older runs -10 15
VIS = re.compile(r"\[enemy\] tgt vis forced ([0-9.]+)")
SEH_TEST = re.compile(r"\[seh\] test: a guarded read of a reserved page (.*?); absorbed (\d+) -> (\d+)")
SEH = re.compile(r"\[seh\] absorbed (\d+) fault\(s\) inside the mod's guards \(last: code (0x[0-9A-Fa-f]+) at (.+?) (0x[0-9A-Fa-f]+)"
                 r"(?:, from (.+?) (0x[0-9A-Fa-f]+), (.+?) (0x[0-9A-Fa-f]+))?\)")
SEH_MAX = 2  # op 10's self-test: two guarded reads of a reserved page
PAIR = re.compile(r"(\w+)=(\S+)")
ARMS = ("TA", "CL", "TE", "TB", "TF0", "TF1", "CH", "CS", "TX")
ENGAGED = ("TB", "TF0", "TF1")  # start on an alerted MULE: their precondition is "-> her", not the calm
CALM_NEEDED = ("TA", "CL", "TE")  # the notice arms: a calm that timed out (or a watch not valid) makes them say nothing


def lines(path):
    if not os.path.exists(path):
        return []
    with open(path, "rb") as f:
        return [raw.decode("utf-8", "replace").rstrip("\r\n") for raw in f]


def fnum(x):
    return float(x) if x is not None else None


def fdash(x):
    """'1.5' -> 1.5, '-' / None -> None"""
    return float(x) if x not in (None, "-") else None


class Game:
    def __init__(self, path):
        self.lines = lines(path)
        self.arms, self.starts, self.tstarts, self.tends, self.checks, self.failed, self.made = {}, {}, {}, {}, [], [], 0
        self.cycles, self.spv, self.faults, self.changes, self.geoms, self.pstim, self.flips7 = None, None, [], [], [], [], []
        self.notmade_guest = 0
        self.sums = dict((k, 0) for k in SUM_NAMES)
        self.summaries = []  # (line, {name: value}) per 5 s / status / arm-end line
        self.redirects = []
        self.sam_steps = []
        self.shoots = []  # (line, id, {...})
        self.vis = []     # (line, value)
        self.seh_tests = []
        self.seh_max, self.seh_last, self.seh_first_over = 0, None, None
        self.armgeo = {}      # label -> (line, text) (run 3: op 11)
        self.places = {}      # (run 4) label -> [op 11 PLACEs sent, kept-in-view PLACEs sent, not sent, the first not-sent reason]
        self.kept = {}        # (run 4) label -> (re-places, why it ended)
        self.place_rx = {}    # (run 4, the guest) label -> [placed, refused]
        self.watches = []     # (line, id, faces her deg, her m, Sam m, PV who, PV age s; the last four None when not read)
        self.calm_ends, self.calm_applies = [], []  # (line, n, id, text)
        self.probe_checks = None
        self.onces = {}       # kind (vt_other, noslot, onhit_bad, mismatch) -> [line, text of the first, count]
        self.has_vt = False   # run 3: the summaries carry "shoot32 vt_other / noslot"
        self.has_sight = False  # M3f: the summaries carry "sight keeps ..."
        self.sights = []      # (M3f) (line, id, {...}) the SIGHT lines
        self.los = []         # (M3f) (line, id, verdict, text)
        self.pickbits = []    # (M3f) (line, bit, on, why)
        self.selftest = None  # (M3f) (ok|failed, text) the first
        self.seen = []        # (M3f) (line, label, seen|not seen, text)
        self.ini_pick = None  # (M3f) the "[config] ... enemy_target_pick=n" key
        self.place_probe = {}  # (M3f) label -> [(chosen, of, skipped, ground dz text, probe text)] per PLACE sent
        for i, line in enumerate(self.lines):
            m = RESULT.search(line)
            if m:
                d = dict(PAIR.findall(m.group(3)))
                d["id"], d["_at"] = m.group(2), i
                self.arms[m.group(1)] = d
                continue
            m = WATCH.search(line)
            if m:
                x = WATCH_X.search(line)
                s = WATCH_S.search(line)
                self.watches.append((i, m.group(1), int(m.group(2))) + ((float(x.group(1)), float(x.group(2)), x.group(4), float(x.group(5)))
                                                                         if x else (None, None, None, None)) +
                                    ((fdash(s.group(1)), s.group(2), fdash(s.group(3))) if s else (None, None, None)))
                continue
            m = SIGHT.search(line)
            if m:
                g = m.groups()
                self.sights.append((i, g[0], {"clear": int(g[1]), "blocked": int(g[2]), "out": int(g[3]), "low_vis": int(g[4]), "keeps": int(g[5]),
                                              "synth": int(g[6]), "skip": int(g[7]), "last": fdash(g[8]), "lead": int(g[9]), "tail": int(g[10]),
                                              "rec": g[11].strip() if g[11] else None}))
                continue
            m = LOS.search(line)
            if m:
                self.los.append((i, m.group(1), m.group(2), m.group(3)))
                continue
            m = PICKBIT.search(line)
            if m:
                self.pickbits.append((i, int(m.group(1)), m.group(2) == "on", m.group(3)))
                continue
            m = SELFTEST.search(line)
            if m:
                if self.selftest is None:
                    self.selftest = (m.group(1), m.group(2))
                continue
            m = SEEN.search(line)
            if m:
                self.seen.append((i, m.group(1), m.group(2), m.group(3)))
                continue
            m = CONFIG_PICK.search(line)
            if m and self.ini_pick is None:
                self.ini_pick = int(m.group(1))
            m = ARMGEO.search(line)
            if m:
                self.armgeo[m.group(1)] = (i, line[m.start():].replace("[enemy] tgt arm-at-geometry ", ""))
                continue
            m = PLACE_SENT.search(line)
            if m:
                self.places.setdefault(m.group(1), [0, 0, 0, None])[0 if m.group(2) == "op 11" else 1] += 1
                x = PLACE_PROBE.search(line)
                if x:
                    self.place_probe.setdefault(m.group(1), []).append((int(x.group(3)), int(x.group(4)), int(x.group(5)), x.group(1), x.group(2)))
                continue
            m = PLACE_NOT.search(line)
            if m:
                pl = self.places.setdefault(m.group(1), [0, 0, 0, None])
                pl[2] += 1
                pl[3] = pl[3] or m.group(2)
                continue
            m = KEPT.search(line)
            if m:
                self.kept[m.group(1)] = (int(m.group(2)), m.group(3))
                continue
            m = PLACE_RX.search(line)
            if m:
                self.place_rx.setdefault(m.group(1), [0, 0])[0 if m.group(2) == ": " else 1] += 1
                continue
            m = CALM_END.search(line)
            if m:
                self.calm_ends.append((i, int(m.group(1)), m.group(2), "%s (%s" % (m.group(3), m.group(4))))
                continue
            m = CALM_APPLY.search(line)
            if m:
                self.calm_applies.append((i, int(m.group(1)), m.group(2), "phase 0 on %s members, the CP %s" % (m.group(3), m.group(4))))
                continue
            m = PROBE_CHECKS.search(line)
            if m:
                self.probe_checks = (int(m.group(1)), m.group(2))
                continue
            m = ONCE.search(line)
            if m:
                t = m.group(1)
                kind = ("vt_other" if "has vtable" in t else "noslot" if "not a registered" in t else
                        "onhit_bad" if t.startswith("onhit_bad") else "onhit_odd" if t.startswith("onhit odd") else "mismatch")
                if kind in self.onces:
                    self.onces[kind][2] += 1
                else:
                    self.onces[kind] = [i, t, 1]
                continue
            m = START.search(line)
            if m:
                self.starts[m.group(1)] = i
                continue
            m = TSTART.search(line)
            if m:
                self.tstarts[m.group(1)] = (i, m.group(3), float(m.group(4)), float(m.group(5)), m.group(6))
                continue
            m = TEND.search(line)
            if m:
                self.tends[m.group(1)] = (i, m.group(3), int(m.group(4)), int(m.group(5)))
                continue
            m = FLIPS7.search(line)
            if m:
                self.flips7.append((i, m.group(1), m.group(2), int(m.group(3)), int(m.group(4))))
                continue
            m = SHOOT.search(line)
            if m:
                g = m.groups()
                self.shoots.append((i, g[0], {"calls": int(g[1]), "redirected": int(g[2]), "points": int(g[3]), "at_her": int(g[4]),
                                              "at_sam": int(g[5]), "at_none": int(g[6]), "last": g[7], "mode": int(g[8]), "age": float(g[9]),
                                              "d_her": float(g[10]), "d_sam": float(g[11]),
                                              "actor": int(g[12]) if g[12] is not None else None,
                                              "place": int(g[13]) if g[13] is not None else None,
                                              "other": int(g[14]) if g[14] is not None else None,
                                              "t36": int(g[15]) if g[15] is not None else None}))
                continue
            if SAM_STEP.search(line):
                self.sam_steps.append(i)
            m = CHECKS.search(line)
            if m:
                self.checks.append((int(m.group(1)), m.group(2)))
            m = FAILED.search(line)
            if m:
                self.failed.append(m.group(1))
            if MADE.search(line):
                self.made += 1
            if NOTMADE_GUEST.search(line):
                self.notmade_guest += 1
            m = CYCLES.search(line)
            if m:
                self.cycles = tuple(int(x) for x in m.groups()[:4]) + (m.group(5),)
            m = SPV.search(line)
            if m:
                self.spv = tuple(int(x) for x in m.groups()[:4]) + (float(m.group(5)), float(m.group(6)), m.group(7))
            m = PSTIM.search(line)
            if m:
                self.pstim.append((i, m.group(1), float(m.group(2))))
            if FAULT.search(line):
                self.faults.append(line)
            m = CHANGE.search(line)
            if m:
                self.changes.append((i, m.group(1), m.group(2), m.group(3)))
            m = GEOM.search(line)
            if m:
                g = m.groups()
                self.geoms.append({"i": i, "id": g[0], "when": g[1], "dz": float(g[2]), "horiz": float(g[3]), "pitch": float(g[4]),
                                   "bearing": float(g[5]), "valid": g[6] == "1", "sam_dz": fnum(g[7]), "sam_horiz": fnum(g[8]),
                                   "sam_bearing": fnum(g[9]), "sam_pitch": fnum(g[10]), "rule": g[11], "subject": g[12], "sep": fnum(g[13])})
            m = SUMMARY.search(line)
            if m:
                vals = dict((k, int(v)) for k, v in zip(SUM_NAMES, m.groups()) if v is not None)
                if "vt_other" in vals:
                    self.has_vt = True
                if "keeps" in vals:
                    self.has_sight = True
                self.summaries.append((i, vals))
                for k, v in vals.items():
                    if k in SUM_LAST:
                        self.sums[k] = v
                    elif k not in ("her", "sam"):
                        self.sums[k] += v
            m = REDIR.search(line)
            if m:
                self.redirects.append("%s (%s): %s" % m.groups())
            m = VIS.search(line)
            if m:
                self.vis.append((i, float(m.group(1))))
            m = SEH_TEST.search(line)
            if m:
                self.seh_tests.append((m.group(1), int(m.group(2)), int(m.group(3))))
            m = SEH.search(line)
            if m:
                n = int(m.group(1))
                t = TS.search(line)
                rec = {"n": n, "t": t.group(1) if t else "?", "code": m.group(2), "at": "%s %s" % (m.group(3), m.group(4)),
                       "from": ("%s %s, %s %s" % (m.group(5), m.group(6), m.group(7), m.group(8))) if m.group(5) else "not logged (an older build)"}
                self.seh_last = rec
                self.seh_max = max(self.seh_max, n)
                if n > SEH_MAX and self.seh_first_over is None:
                    self.seh_first_over = rec

    def geom(self, label):
        """the arm's own geometry line (enemy_tgt 4 logs it with the arm's label)"""
        for g in self.geoms:
            if g["when"] == label:
                return g
        return None

    def first_start(self, label):
        a, b = self.tstarts.get(label), self.starts.get(label)
        cands = [x for x in ((a[0] if a else None), b) if x is not None]
        return min(cands) if cands else None

    def arm_end(self, label):
        """the arm's last line: its result line, else its tgt end line"""
        a = self.arms.get(label)
        if a:
            return a["_at"]
        e = self.tends.get(label)
        return e[0] if e else None

    def window(self, label):
        s0, e0 = self.first_start(label), self.arm_end(label)
        return (s0, e0) if s0 is not None and e0 is not None else None

    def arm_sums(self, label):
        """the 5 s / status / arm-end summary deltas inside the arm (every MULE): (n lines, {name: sum})"""
        w = self.window(label)
        out = dict((k, 0) for k in SUM_NAMES if k not in SUM_LAST and k not in ("her", "sam"))
        n = 0
        if not w:
            return 0, out
        for i, vals in self.summaries:
            if w[0] < i <= w[1]:
                n += 1
                for k in out:
                    out[k] += vals.get(k, 0)
        return n, out

    def arm_shoot(self, label, mid):
        """the arm MULE's shoot line right after the arm's end line (op 8)"""
        e = self.tends.get(label)
        if not e:
            return None
        for i, sid, d in self.shoots:
            if i > e[0] and sid == mid:
                return d
            if i > e[0] + 40:
                break
        return None

    def engaged_her(self, label, mid, until=None):
        """'tgt id=<id> -> her' after the arm's start (and before `until`, else its end)"""
        w = self.window(label)
        if not w:
            return []
        end = until if until is not None else w[1]
        return [ch for ch in self.changes if w[0] < ch[0] < end and ch[1] == mid and ch[2] == "her"]


def num(d, k, default=-1.0):
    try:
        return float(d.get(k, default))
    except (TypeError, ValueError):
        return default


def phases(d, k):
    """'a/b/c' -> [a, b, c] (None: not logged, an older build)"""
    v = d.get(k) if d else None
    if not v:
        return None
    try:
        p = [int(x) for x in v.split("/")]
    except ValueError:
        return None
    return p if len(p) == 3 else None


def tb_sam_step(host):
    s0 = host.first_start("TB")
    return next((i for i in host.sam_steps if s0 is not None and i > s0), None)


def invalid(host, label):
    """why the arm says nothing (None: it counts)"""
    a = host.arms.get(label)
    if not a:
        return "no result line"
    g = host.geom(label)
    if g and not g["valid"]:
        return "its geometry line says valid 0 (rule %s, %s)" % (g["rule"] or "old", g["subject"] or "her")
    if label in ENGAGED:
        if a.get("stim_lost") == "1":
            return "her stimulus lost during the arm"
        until = tb_sam_step(host) if label == "TB" else None
        if label == "TB" and until is None:
            return "no Sam step"
        if not host.engaged_her(label, a.get("id", ""), until):
            return "no 'tgt id=%s -> her' precondition%s" % (a.get("id", "?"), " before the Sam step" if label == "TB" else "")
        if label in ("TF0", "TF1"):
            if not g or g["sam_horiz"] is None:
                return "no geometry line with Sam's place"
            # (review) Sam 40 deg off: a round at him passes ~14 m beside her, so the rays tell them apart. Run 3: op 11 places him
            # 40 deg off its line to her, so the angle at the MULE between them ("sep") is the measure; older lines: his bearing
            # off its facing
            if g["sep"] is not None and g["sep"] >= 0:
                ang, what = g["sep"], "off its line to her (sep)"
            else:
                ang, what = g["sam_bearing"], "off its facing"
            if not (140 <= g["sam_horiz"] <= 210 and ang is not None and 30 <= ang <= 50):
                return "Sam %.1f m away, %.0f deg %s (want 140-210 m, 30-50 deg)" % (g["sam_horiz"], ang if ang is not None else -1, what)
        return None
    if label in CALM_NEEDED:
        if a.get("calm_timeout") == "1":
            return "calm timeout"
        if a.get("valid") != "1":
            return "watch not valid"
        return None
    # (run 3) CH, CS, TX: their calm need not be ready (they are printed, not gated); her stimulus lost says nothing (CS has none)
    if label != "CS" and a.get("stim_lost") == "1":
        return "her stimulus lost during the arm"
    return None


def ts_ms(line):
    m = TS.search(line)
    return float(m.group(1)) * 1000.0 if m else None


def stim_delay(game, label):
    """ms from the arm's watch start (the probe's "L1 arm X ... start" line, else the tgt start line) to the first
    "[enemy] pstim made #" line after it, within the arm (None: no such line)"""
    i0 = game.starts.get(label)
    if i0 is None:
        i0 = game.first_start(label)
    if i0 is None:
        return None
    end = game.arm_end(label)
    t0 = ts_ms(game.lines[i0])
    for i in range(i0 + 1, end if end is not None else len(game.lines)):
        if MADE.search(game.lines[i]):
            t1 = ts_ms(game.lines[i])
            return t1 - t0 if t0 is not None and t1 is not None else None
    return None


def fires_at_her(a):
    f = num(a, "fired", 0)
    return f >= 1 and num(a, "at_her", 0) * 10 >= f * 6


COLS = [("arm", None), ("id", "id"), ("valid", "valid"), ("stim", "stim_start"), ("calm", "calm"), ("notice", "noticed"), ("turn_h", "turned_her"),
        ("turn_s", "turned_sam"), ("appr h/s", None), ("aim h/s/n", None), ("fired", "fired"), ("rays", "rays"), ("at h/s", None), ("pv h/s", None),
        ("switch s/h", None), ("cls h/s", None), ("rec0 h/s", None), ("fired_p", "fired_p"), ("at_her_p", "at_her_p"), ("at_sam_p", "at_sam_p"),
        ("pv/stim age0", None)]


def row(name, a):
    out = []
    for title, key in COLS:
        if title == "arm":
            out.append(name)
        elif key:
            out.append(a.get(key, "-"))
        else:
            pairs = {"appr h/s": ("approached_her", "approached_sam"), "aim h/s/n": ("aim_her", "aim_sam", "aim_n"), "at h/s": ("at_her", "at_sam"),
                     "pv h/s": ("pv_her", "pv_sam"), "switch s/h": ("switch_sam", "switch_her"), "cls h/s": ("cls_her", "cls_sam"),
                     "rec0 h/s": ("rec0_her", "rec0_sam"), "pv/stim age0": ("pv_age0", "stim_age0")}[title]
            out.append("/".join(a.get(k, "-") for k in pairs))
    return out


def shoot_text(d):
    if not d:
        return "no shoot line"
    t = "calls %d redirected %d points %d (at her %d Sam %d none %d); last %s mode %d age %.1f s" % (
        d["calls"], d["redirected"], d["points"], d["at_her"], d["at_sam"], d["at_none"], d["last"], d["mode"], d["age"])
    if d.get("actor") is not None:
        t += "; modes actor %d place %d other %d" % (d["actor"], d["place"], d["other"])
    if d.get("t36") is not None:
        t += "; t36 %d" % d["t36"]
    return t


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    run = argv[1]
    host = Game(os.path.join(run, "game", "stranded_together.log"))
    guest = Game(os.path.join(run, "guest", "game", "stranded_together.log"))
    items = []

    def gate(name, ok, why, inv=None):
        if inv:
            items.append((name, "INVALID", "%s: %s" % (inv, why)))
        else:
            items.append((name, ok if isinstance(ok, str) else ("PASS" if ok else "FAIL"), why))

    for who, g in (("host", host), ("guest", guest)):
        last = g.checks[-1] if g.checks else None
        bad = last is None or "FAILED" in last[1] or g.failed
        gate("checks " + who, not bad, ("%d: %s" % last) if last else "no byte check line")
        gate("spv1 " + who, bool(g.spv) and g.spv[0] > 0 and g.spv[1] > 0,
             ("sent %d recv %d stale %d bad %d; aim ours %.2f m, hers %.2f m (%s)" % g.spv) if g.spv else "no spv1 line")
        gate("faults " + who, not g.faults, g.faults[0] if g.faults else "none")
        t = g.seh_tests[-1] if g.seh_tests else None
        gate("seh test " + who, bool(t) and t[0] == "faulted and was caught",
             ("%s; absorbed %d -> %d" % t) if t else "no '[seh] test:' line")
        over = g.seh_first_over
        if over:
            why = "absorbed %d (the self-test %d); first over %d at %s s: code %s at %s, from %s" % (
                g.seh_max, SEH_MAX, SEH_MAX, over["t"], over["code"], over["at"], over["from"])
        elif g.seh_last:
            why = "absorbed %d (the self-test %d; last at %s, from %s)" % (g.seh_max, SEH_MAX, g.seh_last["at"], g.seh_last["from"])
        else:
            why = "no '[seh] absorbed' line"
        gate("seh " + who, g.seh_max <= SEH_MAX, why)
    gate("made host", host.made > 0, "%d made line(s)" % host.made)
    gate("host only (guest refuses)", guest.made == 0 and guest.notmade_guest > 0,
         "guest made %d, 'not made (the guest (host only))' %d" % (guest.made, guest.notmade_guest))
    c = host.cycles
    gate("cycles", bool(c) and c[1] == c[0] and c[2] == c[0] and c[3] == 0 and c[4].startswith("made again"),
         ("%d: made %d removed %d faults %d; %s" % c) if c else "no cycles line")

    arms = host.arms
    missing = [n for n in ARMS if n not in arms]
    inv = dict((n, invalid(host, n)) for n in ARMS)
    ta, tb, te, cl = arms.get("TA", {}), arms.get("TB", {}), arms.get("TE", {}), arms.get("CL", {})
    # (run 3) a MULE out of a calm often moves first (phase 5) and fires late: the firing goal is gated in TF1 and TB; TA needs the
    # notice and the turn, no round at Sam, and >= 60 % at her only when it fired
    ta_fired = num(ta, "fired", 0)
    gate("TA notice+turn (fire if any)",
         num(ta, "noticed") >= 0 and num(ta, "turned_her") >= 0 and num(ta, "at_sam", 0) == 0 and (ta_fired < 1 or fires_at_her(ta)),
         "noticed %s turned_her %s; fired %s at_her %s at_sam %s" % (ta.get("noticed", "-"), ta.get("turned_her", "-"), ta.get("fired", "-"),
                                                                      ta.get("at_her", "-"), ta.get("at_sam", "-")), inv["TA"])
    # TB sticky: picked her before the Sam step; no switch to Sam between its start and its op 7 marker; no unlogged flip either
    tb_id = tb.get("id", "0202B")
    s0 = host.first_start("TB")
    marker = None
    for g in host.geoms:
        if g["when"] == "op 7" and s0 is not None and g["i"] > s0:
            marker = g["i"]
            break
    sam_step = tb_sam_step(host)
    her_first = host.engaged_her("TB", tb_id, sam_step) if sam_step is not None else []
    to_sam = [ch for ch in host.changes if s0 is not None and marker is not None and s0 < ch[0] < marker and ch[1] == tb_id and ch[2] == "Sam"]
    f7 = next((f for f in host.flips7 if marker is not None and f[0] > marker and f[1] == tb_id), None)
    # (run 3 review) with her current, TargetPick switches her -> Sam only by "hit by" (our Sam never fires here) or "only visible"
    # (her record out of its PlayerVisual bucket for fresh_ms): every real switch reads "only visible", and the MULE turning off
    # her while both are in view is the very failure TB is for. So a logged "-> Sam" between the arm's start and op 7 FAILs
    # "TB sticky her" whatever its rule; the last watch line before it explains it (faces her > 60 deg: it turned off her, the turn
    # redirect did not hold; else her record dropped out of its bucket while it faced her). The arm says nothing (INVALID) only for
    # causes the scenario made: invalid() above, the Sam step not placing him, her not 10-30 m from it at the switch (she moved),
    # and, with no switch and no flip, Sam not in its cone on op 7's line (horiz 10-20 m, bearing <= 30, sep 15-35: else sticky
    # proves nothing).
    tb_note = ""
    if not inv["TB"] and sam_step is not None and "placed" not in host.lines[sam_step]:
        inv["TB"] = "the Sam step did not place him (%s)" % host.lines[sam_step].strip()[-120:]
    if to_sam and not inv["TB"]:
        ch = to_sam[0]
        prior = [w for w in host.watches if s0 < w[0] < ch[0] and w[1] == tb_id]
        last = prior[-1] if prior else None
        faces = last[2] if last else None
        her_d = last[3] if last else None
        if her_d is not None and not (10 <= her_d <= 30):
            inv["TB"] = "her %.1f m from it at the last watch before the switch (want 10-30 m: she moved)" % her_d
        else:
            past = next((w for w in prior if w[2] > 60), None)
            pv = ("; her PV age %.1f s at the first watch past 60 deg (%d deg)" % (past[6], past[2])) if past and past[6] is not None else ""
            if faces is not None and faces > 60:
                tb_note = "; it turned off her (faces her %d deg at the last watch%s): the turn redirect did not hold" % (faces, pv)
            elif faces is not None:
                tb_note = "; her dropped from its view while it faces her %d deg (PV %s age %s s): her record left its PlayerVisual bucket" % (
                    faces, last[5] or "?", ("%.1f" % last[6]) if last[6] is not None else "?")
            else:
                tb_note = "; no watch line before the switch"
    # (M3f) the arm MULE's SIGHT line right after op 7 (its phase-0 numbers: op 4 / 11 reset them at the start)
    sight_tb = None
    if marker is not None:
        sight_tb = next((d for i, sid, d in host.sights if marker < i <= marker + 40 and sid == tb_id), None)
    if to_sam and not inv["TB"] and tb_note and "PlayerVisual bucket" in tb_note:
        last = [w for w in host.watches if s0 < w[0] < to_sam[0][0] and w[1] == tb_id][-1]
        if last[8] is not None:
            tb_note += "; sight at that watch: bucket %s s, los %s %s s" % (
                ("%.1f" % last[7]) if last[7] is not None else "-", last[8], ("%.1f" % last[9]) if last[9] is not None else "-")
        if sight_tb:
            tb_note += "; phase 0 sight: los clear %d blocked %d out %d low_vis %d, keeps %d synth %d skip %d%s" % (
                sight_tb["clear"], sight_tb["blocked"], sight_tb["out"], sight_tb["low_vis"], sight_tb["keeps"], sight_tb["synth"], sight_tb["skip"],
                " (the line of sight never kept her: blocked or out of its view)" if sight_tb["keeps"] == 0 else
                " (kept by the line of sight, then lost: the estimate stopped being clear)")
    g7 = next((g for g in host.geoms if g["i"] == marker), None) if marker is not None else None
    if not to_sam and not inv["TB"] and g7 and not (f7 and f7[3] > 0):
        if g7["sam_horiz"] is None:
            inv["TB"] = "no Sam place on the op 7 line"
        else:
            sep_ok = g7["sep"] is None or g7["sep"] < 0 or 15 <= g7["sep"] <= 35
            if not (10 <= g7["sam_horiz"] <= 20 and g7["sam_bearing"] <= 30 and sep_ok):
                inv["TB"] = "Sam not in its cone at op 7 (horiz %.1f, bearing %.0f, sep %s)" % (
                    g7["sam_horiz"], g7["sam_bearing"], ("%.0f" % g7["sep"]) if g7["sep"] is not None else "-")
            elif not (10 <= g7["horiz"] <= 30):
                inv["TB"] = "her %.1f m from it at op 7 (want 10-30 m)" % g7["horiz"]
    if marker is None:
        why = "no op 7 marker"
    elif sam_step is None:
        why = "no Sam step"
    else:
        why = "-> her before the Sam step: %s; %s; flips at op 7: %s%s" % (
            "yes" if her_first else "NO", ("%d logged switch(es) to Sam (%s)" % (len(to_sam), to_sam[0][3])) if to_sam else "none logged to Sam",
            f7[3] if f7 else "no line", tb_note)
    # (M3f) TB phase 0's precondition (run 175849: her record had left its bucket ~9.7 s before our Sam stepped out, so phase 0 was
    # never "both visible"): the last watch line before the Sam step has her bucket record <= 1.5 s old, or the line of sight
    # clear <= 0.5 s old. Not seen: INVALID, and so the other phase-0 items (sticky, rounds, bit 32); no sight suffix (an older
    # build): INCONCLUSIVE, the others graded as before.
    p0_inv = None
    prior_s = [w for w in host.watches if s0 is not None and sam_step is not None and s0 < w[0] < sam_step and w[1] == tb_id]
    lw = prior_s[-1] if prior_s else None
    seen_l = [x for x in host.seen if x[1] == "TB" and (s0 is None or x[0] < s0)]
    seen_txt = ("; op 11: her %s %s" % (seen_l[-1][2], seen_l[-1][3])) if seen_l else ""
    if sam_step is None or lw is None:
        gate("TB phase 0: both visible", "INCONCLUSIVE", "no watch line before the Sam step" + seen_txt, inv["TB"])
    elif lw[8] is None:
        gate("TB phase 0: both visible", "INCONCLUSIVE", "no sight suffix on the watch line (an older build)" + seen_txt, inv["TB"])
    else:
        bucket, los, los_age = lw[7], lw[8], lw[9]
        ok_p0 = (bucket is not None and bucket <= 1.5) or (los == "clear" and los_age is not None and los_age <= 0.5)
        why_p0 = "at the last watch before the Sam step: her bucket record %s s old, los %s %s s%s" % (
            ("%.1f" % bucket) if bucket is not None else "-", los, ("%.1f" % los_age) if los_age is not None else "-", seen_txt)
        if ok_p0 or inv["TB"]:
            gate("TB phase 0: both visible", True, why_p0, inv["TB"])
        else:
            p0_inv = "her not seen by it when our Sam stepped out (%s)" % why_p0
            gate("TB phase 0: both visible", True, why_p0, "her not seen by it when our Sam stepped out")
    inv_p0 = inv["TB"] or p0_inv
    gate("TB sticky her", marker is not None and bool(her_first) and not to_sam and bool(f7) and f7[3] == 0, why, inv_p0)
    # TB phase 0 (her visible; Sam far, then in view at 15 m): no round at Sam
    fp, hp, sp = phases(tb, "fired_p"), phases(tb, "at_her_p"), phases(tb, "at_sam_p")
    if not fp or not sp:
        gate("TB phase 0: no round at Sam", "INCONCLUSIVE", "no fired_p / at_sam_p on its result line (an older build)", inv_p0)
    else:
        res = "INCONCLUSIVE" if fp[0] == 0 else sp[0] == 0
        gate("TB phase 0: no round at Sam", res, "phase 0 fired %d at her %d at Sam %d" % (fp[0], hp[0] if hp else -1, sp[0]), inv_p0)
    # (run 3) TB phase 0's bit 32: the arm MULE's shoot line right after op 7 (op 4 / 11 reset its counts at the start, so these
    # are phase 0's numbers): with Sam 15 m away and visible, the decisive bit-32 item
    sh_tb = None
    if marker is not None:
        sh_tb = next((d for i, sid, d in host.shoots if marker < i <= marker + 40 and sid == tb_id), None)
    sp0 = sp[0] if sp else 0
    synth0 = sight_tb["synth"] if sight_tb else 0
    sight_txt = ("; sight keeps %d synth %d skip %d" % (sight_tb["keeps"], synth0, sight_tb["skip"])) if sight_tb else ""
    if not sh_tb:
        gate("TB phase 0: bit 32", "INCONCLUSIVE", "no shoot line after the op 7 marker (an older build)", inv_p0)
    elif sh_tb["calls"] == 0 and synth0 > 0:
        # (M3f) her record was written from ours (the game had her as its sensed target) and no SetTargetActor(0) call: graded
        # on where the shots went (M3f review). The job aiming through its place branch at her stimulus is the outcome the synth
        # is for (TF1 run 175849: 30/30 at her, actor 0); a point or a round at Sam FAILs; no point or round at her proves nothing.
        p0_fired = fp[0] if fp else 0
        to_sam0 = sp0 > 0 or sh_tb["at_sam"] > 0 or sh_tb["last"] == "Sam"
        if to_sam0:
            gate("TB phase 0: bit 32", False, "synth wrote her record (%d) but the shot went to Sam (no actor call to redirect); shoot %s; "
                 "phase 0 rounds at Sam %d%s" % (synth0, shoot_text(sh_tb), sp0, sight_txt), inv_p0)
        elif p0_fired == 0 or sh_tb["at_her"] == 0 or not (sh_tb["place"] or 0):
            gate("TB phase 0: bit 32", "INCONCLUSIVE", "synth wrote her record (%d), no actor call, but no point at her or no round in "
                 "phase 0 (fired %d); shoot %s%s" % (synth0, p0_fired, shoot_text(sh_tb), sight_txt), inv_p0)
        else:
            gate("TB phase 0: bit 32", True, "synth kept her (place branch only, no actor call to redirect): synth %d; shoot %s; phase 0 "
                 "fired %d, at Sam 0%s" % (synth0, shoot_text(sh_tb), p0_fired, sight_txt), inv_p0)
    elif sh_tb["calls"] == 0:
        gate("TB phase 0: bit 32", "INCONCLUSIVE", "%s in phase 0; shoot %s%s" % (
            "the type-0x36 branch (no SetTargetActor)" if (sh_tb.get("t36") or 0) > 0 else "no actor-branch call", shoot_text(sh_tb), sight_txt),
             inv_p0)
    else:
        gate("TB phase 0: bit 32", sh_tb["redirected"] > 0 and sh_tb["at_sam"] == 0 and sp0 == 0,
             "shoot %s; phase 0 rounds at Sam %d%s" % (shoot_text(sh_tb), sp0, sight_txt), inv_p0)
    # (M3f) "TB sight" (information): the raycast's self-test, the bit transitions and TB's phase-0 sight numbers. FAIL only when
    # the key has bit 16, TB counts and the self-test is missing or failed (bit 16 never came on)
    pick = host.ini_pick
    if pick is None:
        for ln in lines(os.path.join(run, "session-host-deployed.ini")):
            mm = re.match(r"\s*enemy_target_pick\s*=\s*(\d+)", ln)
            if mm:
                pick = int(mm.group(1))
    st = host.selftest
    bits = ", ".join("%d %s at %s" % (b, "on" if on else "off", ("%.1f s" % (ts_ms(host.lines[i]) / 1000.0)) if ts_ms(host.lines[i]) is not None else "?")
                     for i, b, on, _ in host.pickbits) or "no pick 16 / 32 line"
    los_tb = [x for x in host.los if x[1] == tb_id and s0 is not None and marker is not None and s0 < x[0] < marker]
    why_s = "pick key %s; self-test %s; bits %s; TB phase 0 los changes %d%s%s" % (
        pick if pick is not None else "-", ("%s %s" % st) if st else "none", bits, len(los_tb),
        (" (last: her %s)" % los_tb[-1][2]) if los_tb else "",
        ("; sight clear %d blocked %d out %d low_vis %d keeps %d synth %d skip %d, her bucket last %s s (lead %d tail %d ms)" % (
            sight_tb["clear"], sight_tb["blocked"], sight_tb["out"], sight_tb["low_vis"], sight_tb["keeps"], sight_tb["synth"], sight_tb["skip"],
            ("%.1f" % sight_tb["last"]) if sight_tb["last"] is not None else "-", sight_tb["lead"], sight_tb["tail"]) +
         (("; synth %s" % sight_tb["rec"]) if sight_tb.get("rec") else "")) if sight_tb else "; no SIGHT line")
    if pick is not None and pick & 16 and not inv["TB"] and (st is None or st[0] != "ok"):
        gate("TB sight", False, why_s + " (bit 16 in the key, the raycast's self-test %s)" % ("missing" if st is None else "failed"))
    else:
        gate("TB sight", "INFO", why_s if (pick is not None and pick & 16) else why_s + " (bit 16 not in the key)")
    sw_s, sw_h = num(tb, "switch_sam"), num(tb, "switch_her")
    why_sw = "switch_sam %s; phase 1 fired %s at Sam %s at her %s" % (
        tb.get("switch_sam", "-"), fp[1] if fp else "-", sp[1] if sp else "-", hp[1] if hp else "-")
    if sw_s == -2:
        gate("TB switch to Sam <= 3 s", "INCONCLUSIVE", "phase 0 ended on Sam: no switch to measure; " + why_sw, inv["TB"])
    else:
        gate("TB switch to Sam <= 3 s", 0 <= sw_s <= 3000, why_sw, inv["TB"])
    # TB phase 2: she is back with Sam still in view: stays Sam (no logged switch to her, no round at her)
    tb_end = host.arm_end("TB")
    back = next((i for i, v in host.vis if s0 is not None and tb_end is not None and s0 < i < tb_end and v > 0), None)
    to_her2 = [ch for ch in host.changes if back is not None and back < ch[0] < tb_end and ch[1] == tb_id and ch[2] == "her"]
    if back is None:
        gate("TB phase 2: stays Sam", False, "no 'tgt vis forced' > 0 line in the arm", inv["TB"])
    elif not hp or not sp:
        gate("TB phase 2: stays Sam", "INCONCLUSIVE", "no at_her_p on its result line (an older build); %d logged switch(es) to her" % len(to_her2),
             inv["TB"])
    elif sw_s == -1:
        gate("TB phase 2: stays Sam", "INCONCLUSIVE", "it never switched to Sam in phase 1 (switch_sam %s)" % tb.get("switch_sam", "-"), inv["TB"])
    elif sw_s == -2 and not (sp[1] > 0 and hp[1] == 0):
        gate("TB phase 2: stays Sam", "INCONCLUSIVE", "phase 0 ended on Sam and phase 1 did not fire at Sam only (at Sam %d at her %d)" % (
            sp[1], hp[1]), inv["TB"])
    else:
        gate("TB phase 2: stays Sam", not to_her2 and hp[2] == 0, "%s; phase 2 fired %d at her %d at Sam %d; switch_her %s" % (
            ("%d logged switch(es) to her (%s)" % (len(to_her2), to_her2[0][3])) if to_her2 else "no logged switch to her", fp[2], hp[2],
            sp[2], tb.get("switch_her", "-")), inv["TB"])
    # TF0 (bit 32 off): the actor branch, when the job takes it, resolves to Sam; TF1 (bit 32 on): the fix
    tf0, tf1 = arms.get("TF0", {}), arms.get("TF1", {})
    sh0 = host.arm_shoot("TF0", tf0.get("id", "")) if tf0 else None
    sh1 = host.arm_shoot("TF1", tf1.get("id", "")) if tf1 else None
    f0, s0_ = num(tf0, "fired", 0), num(tf0, "at_sam", 0)
    pt_sam0 = bool(sh0) and (sh0["at_sam"] > 0 or sh0["last"] == "Sam")
    why0 = "fired %d at her %d at Sam %d; shoot %s" % (f0, num(tf0, "at_her", 0), s0_, shoot_text(sh0))
    tf0_note = None
    if sh0 and sh0["redirected"] > 0:
        gate("TF0 bit 32 off: the actor branch at Sam", False, why0 + " (bit 32 was not off)", inv["TF0"])
    elif not sh0:
        gate("TF0 bit 32 off: the actor branch at Sam", "INCONCLUSIVE", why0, inv["TF0"])
    elif sh0["actor"] is None:
        # an older build (no per-mode counts): the run-2 rule, rounds or the point at Sam prove the path
        gate("TF0 bit 32 off: the actor branch at Sam", True if (s0_ > 0 or pt_sam0) else "INCONCLUSIVE", why0 + " (no per-mode counts)", inv["TF0"])
    elif sh0["actor"] == 0:
        tf0_note = "TF0: place branch only (actor 0 place %d): nothing to prove" % sh0["place"]
    else:
        gate("TF0 bit 32 off: the actor branch at Sam", s0_ > 0 or sh0["at_sam"] > 0,
             why0 + ("" if (s0_ > 0 or sh0["at_sam"] > 0) else " (actor branch did not resolve to Sam)"), inv["TF0"])
    f1, h1, s1 = num(tf1, "fired", 0), num(tf1, "at_her", 0), num(tf1, "at_sam", 0)
    why1 = "fired %d at her %d at Sam %d; shoot %s" % (f1, h1, s1, shoot_text(sh1))
    # (review) the shoot line is the direct measure of where the job's aim point went: any at Sam FAILs; with actor-branch calls
    # PASS needs them redirected and the point at her
    pt_sam1 = bool(sh1) and (sh1["at_sam"] > 0 or sh1["last"] == "Sam")
    pt_her1 = bool(sh1) and (sh1["at_her"] > 0 or sh1["last"] == "her")
    calls1 = sh1["calls"] if sh1 else 0
    if s1 > 0 or pt_sam1:
        res1 = False
        if pt_sam1:
            why1 += " (the aim point at Sam)"
    elif f1 < 3:
        res1 = "INCONCLUSIVE"
        why1 += " (fewer than 3 rounds)"
    elif calls1 > 0 and sh1["redirected"] == 0:
        res1 = False
        why1 += " (actor-branch calls, none redirected)"
    elif calls1 == 0:
        # (run 3 review) no actor-branch call: the rounds at her prove nothing about bit 32 (TF0 with bit 32 off fired at her too)
        res1 = "INCONCLUSIVE"
        if not sh1:
            why1 += " (no shoot line: an older build)"
        elif (sh1.get("t36") or 0) > 0:
            why1 += " (the type-0x36 branch: the job never calls SetTargetActor; bit 32 not exercised)"
        else:
            why1 += " (no actor-branch call: bit 32 not exercised (modes actor %s place %s))" % (
                sh1["actor"] if sh1["actor"] is not None else "-", sh1["place"] if sh1["place"] is not None else "-")
    else:
        res1 = h1 * 3 >= f1 * 2 and sh1["redirected"] > 0 and pt_her1
    gate("TF1 bit 32 on: no round at Sam", res1, why1, inv["TF1"])
    # (run 3) bit 32's carrier filter: a carrier vtable neither the MULE's nor the NPC's FAILs (a class we did not anticipate).
    # (Review) the counters exist only inside the job's id-0 call branch: no call there says nothing (INCONCLUSIVE); every call
    # dropped as noslot, or noslot over a small share of them, FAILs (a MULE whose calls are never redirected); a few noslot calls
    # INCONCLUSIVE (with the once-line)
    once = host.onces
    sh_calls_all, t36_all = host.sums["sh_calls"], host.sums["t36"]
    if not host.has_vt:
        gate("bit 32 filter", "INCONCLUSIVE", "no 'shoot32 vt_other / noslot' on the summaries (an older build)")
    else:
        vt_o, nos = host.sums["vt_other"], host.sums["noslot"]
        why_f = "calls %d vt_other %d noslot %d t36 %d%s" % (sh_calls_all, vt_o, nos, t36_all,
                                                           ("; first: %s" % once["vt_other"][1]) if "vt_other" in once else "")
        if "noslot" in once:
            why_f += "; first noslot: %s" % once["noslot"][1]
        elif nos > 0:
            why_f += "; no once-line explains the noslot calls"
        if vt_o > 0:
            gate("bit 32 filter", False, why_f)
        elif nos > 0 and sh_calls_all == 0:
            gate("bit 32 filter", False, why_f + " (every shoot-job call dropped as noslot)")
        elif nos > max(3, sh_calls_all // 10):
            gate("bit 32 filter", False, why_f + " (noslot over a tenth of the calls)")
        elif sh_calls_all == 0:
            gate("bit 32 filter", "INCONCLUSIVE", why_f + (" (no job call reached the hook: the type-0x36 branch)" if t36_all > 0 else
                                                          " (no job call reached the hook)"))
        elif nos > 0:
            gate("bit 32 filter", "INCONCLUSIVE", why_f + " (a few noslot calls)")
        else:
            gate("bit 32 filter", True, why_f)
    # (run 3 review) bit 32 live (docs 16.6): the job's actor-branch calls seen and redirected over TF1 and TB (the arms with bit 32
    # on); calls 0 with the type-0x36 branch counted: INCONCLUSIVE (that weapon never calls SetTargetActor); calls 0 otherwise FAILs
    if not host.has_vt:
        gate("bit 32 live", "INCONCLUSIVE", "no 'shoot32 vt_other / noslot' on the summaries (an older build)")
    else:
        lc = lr = lp = 0
        for label in ("TF1", "TB"):
            _, w = host.arm_sums(label)
            lc, lr, lp = lc + w["sh_calls"], lr + w["sh_redir"], lp + w["sh_pts"]
        why_l = "TF1+TB: calls %d redirected %d points %d; run t36 %d" % (lc, lr, lp, t36_all)
        if lc == 0 and t36_all > 0:
            gate("bit 32 live", "INCONCLUSIVE", why_l + " (the type-0x36 branch: the job never calls SetTargetActor)")
        else:
            gate("bit 32 live", lc > 0 and lr > 0, why_l + ("" if lc > 0 else " (the detour never saw a job call)"))
    # TE: her crouched visibility at the arm's start, the classifier saw her; the timing is printed only
    sa, se = host.tstarts.get("TA"), host.tstarts.get("TE")
    va, ve = (sa[2] if sa else None), (se[2] if se else None)
    na, ne = num(ta, "noticed"), num(te, "noticed")
    lower = ve is not None and va is not None and ve <= 0.7 and ve < va
    gate("TE crouched: lower vis, seen", lower and num(te, "cls_her", 0) > 0,
         "arm-start vis TA %s TE %s; cls_her %s; notice TA %s TE %s (TE > TA + 1 s or none: %s)" % (
             ("%.2f" % va) if va is not None else "-", ("%.2f" % ve) if ve is not None else "-", te.get("cls_her", "-"), ta.get("noticed", "-"),
             te.get("noticed", "-"), "yes" if (ne < 0 or (na >= 0 and ne > na + 1000)) else "no"), inv["TE"])
    gcl = host.geom("CL")
    if gcl:
        n_cl = num(cl, "noticed")
        # (review) the watch starts before the stimulus is made (enemy_tgt 4, then 1 1): the notice is timed from the stimulus
        dly = stim_delay(host, "CL")
        adj = n_cl - dly if n_cl >= 0 and dly is not None else n_cl
        why = "geometry dz %.2f horiz %.1f pitch %.1f bearing %.0f valid %d; noticed %s, stimulus made %s ms after the start" % (
            gcl["dz"], gcl["horiz"], gcl["pitch"], gcl["bearing"], 1 if gcl["valid"] else 0, cl.get("noticed", "-"),
            ("%.0f" % dly) if dly is not None else "-")
        if n_cl >= 0 and dly is not None:
            why += " (%.0f ms after the stimulus)" % adj
        pcl = host.place_probe.get("CL")
        if pcl:
            ch, of, sk, gz, pt = pcl[-1]
            why += "; last PLACE chose %d of %d, skipped %d raised, ground dz %s (probe %s)" % (ch, of, sk, gz, pt)
        if dly is None:
            res = "INCONCLUSIVE"
            why += "; no 'pstim made' line in the arm"
        elif dly > 1000:
            res = "INCONCLUSIVE"
            why += "; the stimulus over 1 s late"
        elif n_cl >= 0 and adj < -250:
            res = "INCONCLUSIVE"
            why += "; noticed before the stimulus was made"
        elif n_cl >= 0 and adj <= 2000 and abs(gcl["dz"]) <= 1.0:
            res = True
        elif n_cl < 0 and gcl["bearing"] > 45:
            res = "INCONCLUSIVE"
            why += "; outside the melee cone"
        else:
            res = False
        gate("CL level: notice <= 2 s", res, why, inv["CL"])
    else:
        gate("CL level: notice <= 2 s", False, "no geometry line", inv["CL"])
    # (review) the run-2 synthesis's hygiene: the soft counters 0, every round attributed by its ray
    for who, g in (("host", host), ("guest", guest)):
        bad = [k for k in SOFT if g.sums[k] > 0]
        why_soft = ", ".join("%s %d" % (k, g.sums[k]) for k in SOFT)
        if g.sums["onhit_bad"] > 0:
            # (run 3) the first chain's once-line must be there when any occurred
            why_soft += ("; first: %s" % g.onces["onhit_bad"][1]) if "onhit_bad" in g.onces else "; no 'tgt onhit_bad first' line (an older build?)"
        gate("soft " + who, "INCONCLUSIVE" if bad else "PASS", why_soft)
        if g.has_sight:
            # (M3f) an attacker of the game's whose +0x338 is not an object of the game's: information only
            gate("onhit_odd " + who, "INFO", "%d%s" % (g.sums["onhit_odd"], ("; first: %s" % g.onces["onhit_odd"][1]) if "onhit_odd" in g.onces else ""))
    for label in ("TA", "TB", "TF0", "TF1"):
        a = arms.get(label, {})
        if a.get("fired") is None or a.get("rays") is None:
            gate("rays " + label, "INCONCLUSIVE", "no fired / rays on its result line", inv[label])
            continue
        same = num(a, "rays") == num(a, "fired")
        gate("rays " + label, "PASS" if same else "INCONCLUSIVE",
             "rays %s fired %s%s" % (a["rays"], a["fired"], "" if same else " (the rest attributed by the sample line)"), inv[label])

    if tf0_note:
        print(tf0_note)
    for r in host.redirects:
        print("host: tgt redirect %s" % r)
    for who, g in (("host", host), ("guest", guest)):
        if g.checks:
            print("%s: tgt byte checks (%d): %s" % ((who,) + g.checks[-1]))
        for f in g.failed:
            print("%s: byte check FAILED: %s" % (who, f))
        if g.probe_checks:
            print("%s: probe L1 byte checks (%d): %s" % ((who,) + g.probe_checks))
    # (run 3) the calms: the apply line (the CP forced or at rest) and the end line (ready / timeout, blocked by ...)
    applies = dict((n, t) for _, n, _, t in host.calm_applies)
    n_ready = sum(1 for _, _, _, t in host.calm_ends if t.startswith("ready"))
    print("host calms: %d ready of %d" % (n_ready, len(host.calm_ends)))
    for _, n, cid, t in host.calm_ends:
        print("  calm #%d id=%s: %s | %s" % (n, cid, applies.get(n, "no apply line"), t))
    for kind in ("vt_other", "noslot", "onhit_bad", "onhit_odd", "mismatch"):
        if kind in host.onces:
            print("host once %s (%d): %s" % (kind, host.onces[kind][2], host.onces[kind][1]))
    table = [[t for t, _ in COLS]]
    for n in ARMS:
        if n in arms:
            table.append(row(n, arms[n]))
    widths = [max(len(r[i]) for r in table) for i in range(len(COLS))]
    print()
    for r in table:
        print("  ".join(v.ljust(widths[i]) for i, v in enumerate(r)))
    print()
    for label in ARMS:
        g = host.geom(label)
        s = host.tstarts.get(label)
        e = host.tends.get(label)
        a = arms.get(label)
        geo = ""
        if g:
            geo = "geometry her dz %.2f horiz %.1f pitch %.1f bearing %.0f valid %d" % (g["dz"], g["horiz"], g["pitch"], g["bearing"], 1 if g["valid"] else 0)
            if g["sam_horiz"] is not None:
                geo += ", Sam dz %.2f horiz %.1f bearing %.0f" % (g["sam_dz"], g["sam_horiz"], g["sam_bearing"])
            if g["sep"] is not None:
                geo += ", sep %.0f" % g["sep"]
            geo += " (rule %s, %s); " % (g["rule"] or "old", g["subject"] or "her")
        print("%s: %s%s%s%s" % (
            label, geo, ("start vis %.2f aim %.2f m (%s); " % (s[2], s[3], s[4])) if s else "",
            ("end pick %s flips %d gap max %d ms; " % (e[1], e[2], e[3])) if e else "", ("INVALID (%s)" % inv[label]) if inv[label] else "counts"))
        ag = host.armgeo.get(label)
        if ag:
            print("    op 11: %s" % ag[1])
        for _, lab, what, txt in host.seen:
            if lab == label:
                print("    op 11 (M3f): her %s %s" % (what, txt))
        for ch, of, sk, gz, pt in host.place_probe.get(label, []):
            print("    PLACE ground probe (M3f): chose %d of %d, skipped %d raised, ground dz %s (probe %s)" % (ch, of, sk, gz, pt))
        pl, kv, rx = host.places.get(label), host.kept.get(label), guest.place_rx.get(label)
        if pl or kv or rx:
            print("    op 11 PLACEs (run 4): sent %d at the start, %d kept in view%s; kept in view %s; the guest placed %d refused %d" % (
                pl[0] if pl else 0, pl[1] if pl else 0, (", %d not sent (%s)" % (pl[2], pl[3])) if pl and pl[2] else "",
                ("%d re-places (%s)" % kv) if kv else "-", rx[0] if rx else 0, rx[1] if rx else 0))
        if a:
            n, w = host.arm_sums(label)
            print("    redirects in the arm (%d summaries, every MULE): r1 %d r2a %d r2b %d r4 %d veh %d; bit 32 calls %d redirected %d points %d "
                  "(at her %d Sam %d none %d)" % (n, w["r1"], w["r2a"], w["r2b"], w["r4"], w["veh"], w["sh_calls"], w["sh_redir"], w["sh_pts"],
                                                  w["pt_her"], w["pt_sam"], w["pt_none"]))
            print("    %s bit 32 (its shoot line at the arm's end): %s; rounds fired %s rays %s" % (
                a.get("id", "?"), shoot_text(host.arm_shoot(label, a.get("id", ""))), a.get("fired", "-"), a.get("rays", "-")))
    s = host.sums
    print()
    print("host totals (%d summaries): %s" % (len(host.summaries), ", ".join("%s %d" % (k, s[k]) for k in SUM_NAMES if k not in ("her", "sam"))))
    for who, g in (("host", host), ("guest", guest)):
        print("%s soft counters (totals; the 'soft' item is INCONCLUSIVE on any): %s" % (who, ", ".join("%s %d" % (k, g.sums[k]) for k in SOFT)))
    tx = arms.get("TX")
    if tx:
        print("control TX (redirects off): noticed %s turned_her %s fired %s at_her %s at_sam %s (TA: %s / %s / %s / %s)" % (
            tx.get("noticed", "-"), tx.get("turned_her", "-"), tx.get("fired", "-"), tx.get("at_her", "-"), tx.get("at_sam", "-"),
            ta.get("noticed", "-"), ta.get("turned_her", "-"), ta.get("fired", "-"), ta.get("at_her", "-")))
    for n in ("CH", "CS"):
        a = arms.get(n)
        if a:
            print("%s: noticed %s turned_her %s turned_sam %s cls h/s %s/%s app h/s %s/%s" % (
                n, a.get("noticed", "-"), a.get("turned_her", "-"), a.get("turned_sam", "-"), a.get("cls_her", "-"), a.get("cls_sam", "-"),
                a.get("app_her", "-"), a.get("app_sam", "-")))
    print()
    width = max(len(n) for n, _, _ in items)
    for name, st, why in items:
        print("%s  %s  %s" % (name.ljust(width), st.ljust(12), why))
    if missing:
        print("missing arms: %s" % " ".join(missing))
    failed = [n for n, st, _ in items if st == "FAIL"]
    open_items = [n for n, st, _ in items if st in ("INVALID", "INCONCLUSIVE")]
    if failed or missing:
        print("gate: FAIL (%s)" % ", ".join(failed + ["missing " + m for m in missing]))
        return 1
    if open_items:
        print("gate: INCOMPLETE (%s)" % ", ".join(open_items))
        return 3
    print("gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
