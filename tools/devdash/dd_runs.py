"""Hidden test runs: supervisor.log parser, run state classifier, run index, live view.

Sources (docs/devdash.md): <root>\\<yyyyMMdd-HHmmss>-<scenario>\\ with supervisor.log (written live,
one line per WriteFile), summary.json (written once at the end), scenario.txt, and after the games
exit: game\\, guest\\game\\, automation\\*.png, guest\\automation\\*.png. While a run is live the game
logs and screenshots are only in the game folder (<game>\\stranded_together\\...), which the run's
supervisor names on its plan line; devdash reads them there only while that run is live.
"""
import hashlib
import json
import os
import re
import shutil
import threading
import time
from collections import OrderedDict

from dd_util import (clip, first_line, local_iso_ms, local_stamp_ms, read_bytes, read_head, read_json, read_text,
                     stat_key, today_prefix)

REC = re.compile(r'^\[\s*(\d+\.\d{3})\] (.*)$')
HDR = re.compile(r'^st_testenv_supervisor \(pid (\d+)\) started (\S+)')
PLAN = re.compile(r'^game (.*), mod (.*), scenario (.*?) \((\d+) steps, role (\S+), steps limit (\d+) s, '
                  r'game lifetime cap (\d+) s\)$')
STEP = re.compile(r'^step line (\d+) (\??)(.*?) -> (ok|FAIL) \((\d+(?:\.\d+)?) s\): ?(.*)$')
FINAL = re.compile(r'^run (PASSED|FAILED|STOPPED: SAFETY VIOLATION) \(exit (-?\d+)\)')
PROBE_FINAL = re.compile(r'^probe run (PASSED|FAILED)')
REFUSED = re.compile(r'^REFUSED: (.*)$')
USAGE = re.compile(r'^usage error: (.*)$')
RESTORE_REQ = re.compile(r'^restore requested for (.*)$')
DS_STARTED = re.compile(r'^started (.*ds\.exe) pid (\d+) on ')
SCN_STOP = re.compile(r'^scenario (completed|stopped)(?:: (.*))?$')
PHASES = [  # (regex, phase) in the order a run passes them; the last match wins
    (re.compile(r'^game .* steps, role'), 'preparing'),
    (re.compile(r'^started .*ds\.exe pid'), 'booting'),
    (re.compile(r'^(guest )?saves redirect: CONFIRMED'), 'booting'),
    (re.compile(r'^step line '), 'steps'),
    (re.compile(r'^scenario (completed|stopped)'), 'finishing'),
    (re.compile(r'^(guest )?ds\.exe exit code'), 'collecting'),
    (re.compile(r'^restore: |^game folder restore'), 'restoring'),
    (re.compile(r'^Steam cloud_log'), 'checking'),
    (re.compile(r'^saves: (UNCHANGED|\d+ CHANGE)'), 'checking'),
]
NAME = re.compile(r'^(\d{8}-\d{6})-(.+)$')
SAFE_NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,159}$')
SAFE_PNG = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,159}\.png$', re.IGNORECASE)
ROOTKEY = re.compile(r'^[0-9a-f]{10}$')
STARTING_GRACE_S = 120
GAME_FOLDER_PHASES = ('booting', 'steps')  # a live run's phases in which devdash may read the game folder
VERDICT_KEYS = ('mode', 'result', 'end_reason', 'game_exit_code', 'guest_exit_code', 'saves_unchanged', 'saves_ok',
                'restored', 'violation', 'failures', 'two_games', 'error_txt', 'game_alive_s', 'guest_alive_s',
                'seed_saves', 'guest_seed', 'launcher_ini', 'sandbox_session', 'reason')


def parse_log(text):
    """supervisor.log -> segments. A segment starts at the supervisor header (a run or probe) or at
    'restore requested for' (Start-TestRun's re-restore or Restore-TestRun.ps1 append to the same
    file, with the time reset to 0). A line that is not a record continues the previous record."""
    segs, cur = [], None
    for raw in text.split('\n'):
        line = raw.rstrip('\r')
        m = REC.match(line)
        if not m:
            if cur and cur['records'] and line:
                cur['records'][-1][1] += '\n' + line
            continue
        t, msg = float(m.group(1)), m.group(2)
        h = HDR.match(msg)
        rr = RESTORE_REQ.match(msg)
        if h or rr or cur is None:
            cur = {'kind': 'restore' if rr else 'run', 'pid': int(h.group(1)) if h else None,
                   'started': h.group(2) if h else None, 'records': [], 'steps': [], 'plan': None,
                   'final': None, 'exit': None, 'phase': 'starting', 'refused': None, 't_last': 0.0,
                   'ds_pids': [], 'phase_times': {}, 'stop': None, 'violation_text': None}
            segs.append(cur)
        cur['records'].append([t, msg])
        cur['t_last'] = t
        p = PLAN.match(msg)
        if p:
            cur['plan'] = {'game_dir': p.group(1), 'scenario_path': p.group(3), 'steps_total': int(p.group(4)),
                           'role': p.group(5), 'steps_limit_s': int(p.group(6)), 'lifetime_cap_s': int(p.group(7))}
        s = STEP.match(msg)
        if s:
            cur['steps'].append({'line': int(s.group(1)), 'optional': s.group(2) == '?', 'what': s.group(3),
                                 'ok': s.group(4) == 'ok', 'seconds': float(s.group(5)), 'detail': s.group(6),
                                 't': t})
        d = DS_STARTED.match(msg)
        if d:
            cur['ds_pids'].append(int(d.group(2)))
        sc = SCN_STOP.match(msg)
        if sc:
            cur['stop'] = sc.group(2) or sc.group(1)
        if msg.startswith('SAFETY VIOLATION: '):
            cur['violation_text'] = msg[len('SAFETY VIOLATION: '):]
        for rx, ph in PHASES:
            if rx.match(msg):
                cur['phase'] = ph
                cur['phase_times'].setdefault(ph, t)
        f = FINAL.match(msg) or PROBE_FINAL.match(msg)
        if f:
            cur['final'] = f.group(1)
            if f.re is FINAL:
                cur['exit'] = int(f.group(2))
            cur['phase'] = 'done'
        r = REFUSED.match(msg) or USAGE.match(msg)
        if r:
            cur['refused'] = r.group(1)
            cur['final'] = 'REFUSED'
            cur['phase'] = 'done'
    return segs


def state_of_final(final):
    return {'PASSED': 'passed', 'FAILED': 'failed', 'REFUSED': 'refused'}.get(final, 'violation')


def scenario_subtitle(run_dir):
    """The first '#' line of scenario.txt (what the scenario is for)."""
    h = read_head(os.path.join(run_dir, 'scenario.txt'), 2048)
    if not h:
        return ''
    for ln in h.split('\n'):
        ln = ln.strip()
        if ln.startswith('#'):
            return clip(ln.lstrip('#').strip(), 300)
        if ln:
            break
    return ''


def scenario_line(run_dir, after_line):
    """The next scenario step after `after_line` (step lines are scenario.txt line numbers)."""
    t = read_text(os.path.join(run_dir, 'scenario.txt'), 512 * 1024)
    if not t:
        return None
    lines = t.split('\n')
    for i in range(max(after_line, 0), len(lines)):
        s = lines[i].strip()
        if s and not s.startswith('#'):
            return {'line': i + 1, 'what': clip(s, 300)}
    return None


def verdict_of(summary):
    if not isinstance(summary, dict):
        return None
    v = {k: summary.get(k) for k in VERDICT_KEYS if k in summary}
    v['cloud_uploads'] = len(summary.get('cloud_uploads') or [])
    v['game_server_writes'] = len(summary.get('game_server_writes') or [])
    if 'online_blocked' in summary:
        v['online_blocked'] = summary.get('online_blocked')
    issues = []
    if summary.get('mode') == 'game' and summary.get('result') not in (None, 'refused'):
        if summary.get('game_exit_code') not in (0, None):
            issues.append('game exit %s' % summary.get('game_exit_code'))
        if summary.get('two_games') and summary.get('guest_exit_code') not in (0, None):
            issues.append('guest exit %s' % summary.get('guest_exit_code'))
        if summary.get('saves_unchanged') is False:
            issues.append('REAL SAVES CHANGED')
        if summary.get('saves_ok') is False:
            issues.append('saves not verified')
        if summary.get('restored') is False:
            issues.append('game folder not restored')
        if summary.get('violation'):
            issues.append('safety violation')
        if v['cloud_uploads']:
            issues.append('%d cloud upload(s)' % v['cloud_uploads'])
        if v['game_server_writes']:
            issues.append('%d game server write(s)' % v['game_server_writes'])
        if summary.get('failures'):
            issues.append('%s failed step(s)' % summary.get('failures'))
    v['issues'] = issues
    return v


def hold_state(path):
    """A held session's state from <run>\\progress.jsonl (supervisor --hold): 'ready' while it takes steps
    from <run>\\inbox, 'end' after; None for an ordinary run. The file is append-only (safe to read)."""
    t = read_text(path, 256 * 1024)
    state = None
    for ln in (t or '').split('\n'):
        if '"hold"' not in ln:
            continue
        try:
            j = json.loads(ln)
        except ValueError:
            continue
        if j.get('event') == 'hold':
            state = j.get('state') or state
    return state


def needs_restore(run_dir):
    """Same rule as Start-TestRun.ps1: a manifest without an 'ok' completion marker."""
    def bad(manifest, marker):
        if not os.path.exists(os.path.join(run_dir, manifest)):
            return False
        return first_line(os.path.join(run_dir, marker)) != 'ok'
    return bad('restore.txt', 'restored.txt') or bad('saves-restore.txt', 'saves-restored.txt')


def rootkey(path):
    return hashlib.sha1(os.path.normcase(os.path.abspath(path)).encode('utf-8')).hexdigest()[:10]


def _list_pngs(d):
    out = []
    try:
        with os.scandir(d) as it:
            for e in it:
                if e.is_file() and SAFE_PNG.match(e.name):
                    st = e.stat()
                    out.append((e.name, st.st_size, int(st.st_mtime * 1000)))
    except OSError:
        pass
    out.sort(key=lambda x: (x[2], x[0]))
    return out


def parse_status_txt(text):
    out = {}
    for ln in (text or '').split('\n'):
        if '=' in ln:
            k, v = ln.split('=', 1)
            k = k.strip()
            if k and len(out) < 200:
                out[k] = clip(v.strip(), 400)
    return out


class RunStore:
    """Index over every runs root. Briefs are cached by the stat of the files they come from;
    only a live run's log is re-parsed on each poll (incremental by size)."""

    def __init__(self, repo, state_dir, procs, extra_roots=(), now=None):
        self.repo = repo
        self.state_dir = state_dir
        self.procs = procs
        self._now = now
        self._lock = threading.RLock()
        self._extra = [os.path.abspath(r) for r in extra_roots]
        self._remembered = self._load_roots()
        self._names = {}        # rootkey -> (time, [names])
        self._briefs = {}       # (rootkey, name) -> (statkeys, brief_core)
        self._full = OrderedDict()  # (rootkey, name) -> (logkey, segs)
        self._live_cache = {}   # path -> (time, value)

    # ---- roots
    def now(self):
        return self._now() if self._now else time.time()

    def _roots_file(self):
        return os.path.join(self.state_dir, 'roots.json') if self.state_dir else None

    def _load_roots(self):
        p = self._roots_file()
        j = read_json(p) if p else None
        return [r for r in (j or {}).get('roots', []) if isinstance(r, str)]

    def remember_root(self, path):
        """A runs root seen in a live supervisor's --run-dir or a status file's run_dir."""
        if not path:
            return
        path = os.path.abspath(path)
        if os.path.basename(path).lower() != 'runs' or not os.path.isdir(path):
            return
        with self._lock:
            known = {os.path.normcase(r) for r in self._remembered + self._extra}
            if os.path.normcase(path) in known:
                return
            self._remembered.append(path)
            p = self._roots_file()
            if p:
                try:
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    with open(p + '.tmp', 'w', encoding='utf-8') as f:
                        json.dump({'roots': self._remembered}, f, indent=1)
                    os.replace(p + '.tmp', p)
                except OSError:
                    pass

    def roots(self):
        found = []
        if self.repo:
            try:
                for d in sorted(os.listdir(self.repo)):
                    if d.lower().startswith('build'):
                        r = os.path.join(self.repo, d, 'runs')
                        if os.path.isdir(r):
                            found.append(r)
            except OSError:
                pass
        found += self._extra + self._remembered
        out, seen = OrderedDict(), set()
        for r in found:
            k = rootkey(r)
            if k not in seen and os.path.isdir(r):
                seen.add(k)
                out[k] = r
        return out

    def root_label(self, path):
        if self.repo:
            try:
                rel = os.path.relpath(path, self.repo)
                if not rel.startswith('..'):
                    return rel.replace('\\', '/')
            except ValueError:
                pass
        return path

    def names(self, rk, root):
        with self._lock:
            c = self._names.get(rk)
            if c and time.time() - c[0] < 2.0:
                return c[1]
        try:
            names = [d for d in os.listdir(root) if SAFE_NAME.match(d) and os.path.isdir(os.path.join(root, d))]
        except OSError:
            names = []
        with self._lock:
            self._names[rk] = (time.time(), names)
        return names

    def resolve(self, rk, name):
        """Run folder for (rootkey, name), or None. No free paths: the name must be listed in a known root."""
        if not ROOTKEY.match(rk or '') or not SAFE_NAME.match(name or ''):
            return None
        root = self.roots().get(rk)
        if not root:
            return None
        if name not in self.names(rk, root):
            self._names.pop(rk, None)
            if name not in self.names(rk, root):
                return None
        p = os.path.realpath(os.path.join(root, name))
        if os.path.dirname(p).lower() != os.path.realpath(root).lower():
            return None
        return p

    def find_run_dir(self, run_dir):
        """(rootkey, name) for an absolute run folder path (status files name runs by path)."""
        if not run_dir:
            return None
        parent, name = os.path.split(os.path.abspath(run_dir.rstrip('\\/')))
        self.remember_root(parent)
        rk = rootkey(parent)
        if rk in self.roots() and SAFE_NAME.match(name):
            return rk, name
        return None

    # ---- parsing and classification
    def _segments(self, rk, name, run_dir):
        lp = os.path.join(run_dir, 'supervisor.log')
        k = stat_key(lp)
        if k is None:
            return None
        with self._lock:
            c = self._full.get((rk, name))
            if c and c[0] == k:
                self._full.move_to_end((rk, name))
                return c[1]
        b = read_bytes(lp)
        if b is None:
            return None
        segs = parse_log(b.decode('utf-8', 'replace'))
        with self._lock:
            self._full[(rk, name)] = (k, segs)
            while len(self._full) > 12:
                self._full.popitem(last=False)
        return segs

    def _core(self, rk, name, run_dir):
        """The cacheable part of a brief (everything except liveness)."""
        lp = os.path.join(run_dir, 'supervisor.log')
        sp = os.path.join(run_dir, 'summary.json')
        pj = os.path.join(run_dir, 'progress.jsonl')
        keys = (stat_key(lp), stat_key(sp), stat_key(os.path.join(run_dir, 'restored.txt')),
                stat_key(os.path.join(run_dir, 'saves-restored.txt')), stat_key(pj))
        with self._lock:
            c = self._briefs.get((rk, name))
            if c and c[0] == keys:
                return c[1]
        m = NAME.match(name)
        core = {'name': name, 'rootkey': rk, 'stamp': local_stamp_ms(m.group(1)) if m else None,
                'scenario': m.group(2) if m else name, 'has_log': keys[0] is not None}
        if keys[0] is not None:
            b = read_bytes(lp)
            segs = parse_log(b.decode('utf-8', 'replace')) if b is not None else []
            run = next((s for s in segs if s['kind'] == 'run'), segs[0] if segs else None)
            if run:
                plan = run['plan'] or {}
                core.update({
                    'pid': run['pid'], 'final': run['final'], 'exit': run['exit'], 'refused': run['refused'],
                    'phase': run['phase'], 'started': local_iso_ms(run['started']) if run['started'] else None,
                    'duration_s': round(run['t_last'], 1), 'steps_done': len(run['steps']),
                    'steps_total': plan.get('steps_total'), 'role': plan.get('role'),
                    'two_games': plan.get('role') == 'host', 'steps_limit_s': plan.get('steps_limit_s'),
                    'game_dir': plan.get('game_dir'), 'ds_pids': run['ds_pids'],
                    'failed_lines': [s['line'] for s in run['steps'] if not s['ok'] and not s['optional']],
                    'optional_failed': [s['line'] for s in run['steps'] if not s['ok'] and s['optional']],
                    'last_step': dict(run['steps'][-1]) if run['steps'] else None, 'stop': run['stop'],
                    'violation_text': run['violation_text'],
                    'repaired_later': any(s['kind'] == 'restore' for s in segs),
                    'probe': bool(run['final'] and run['final'].startswith('probe')) or name.endswith('-probe'),
                })
                if core['last_step']:
                    core['last_step']['detail'] = clip(core['last_step']['detail'] or '', 400)
        core['hold'] = hold_state(pj) if keys[4] is not None else None
        if keys[1] is not None:
            core['verdict'] = verdict_of(read_json(sp))
        else:
            core['verdict'] = None
        core['needs_restore_files'] = needs_restore(run_dir)
        core['subtitle'] = scenario_subtitle(run_dir)
        core['shots'] = len(_list_pngs(os.path.join(run_dir, 'automation'))) + \
            len(_list_pngs(os.path.join(run_dir, 'guest', 'automation')))
        with self._lock:
            self._briefs[(rk, name)] = (keys, core)
        return core

    def brief(self, rk, name, run_dir=None, sup_pids=None):
        run_dir = run_dir or self.resolve(rk, name)
        if not run_dir:
            return None
        core = self._core(rk, name, run_dir)
        b = dict(core)
        if sup_pids is None:
            sup_pids = set(self.procs.pids('st_testenv_supervisor.exe'))
        if not core.get('has_log'):
            try:
                age = self.now() - os.path.getmtime(run_dir)
            except OSError:
                age = 1e9
            b['state'] = 'starting' if age < STARTING_GRACE_S else 'aborted-before-start'
        elif core.get('final'):
            b['state'] = state_of_final(core['final'])
        elif core.get('pid') in sup_pids:
            b['state'] = 'live'
        else:
            b['state'] = 'interrupted'
        b['needs_restore'] = b['state'] not in ('live', 'starting') and core.get('needs_restore_files', False)
        # The game folder is read only while the games run (booting, steps): from 'finishing' on the supervisor collects,
        # deletes and restores those files, and a read then could make a delete or a replace fail (an INCOMPLETE restore).
        b['game_live'] = b['state'] == 'live' and core.get('phase') in GAME_FOLDER_PHASES
        if b['state'] == 'live' and core.get('started'):
            b['elapsed_s'] = round(max(0.0, self.now() - core['started'] / 1000.0), 1)
        st, sd = b.get('steps_total'), b.get('steps_done') or 0
        b['pct'] = round(min(100.0, 100.0 * sd / st), 1) if st else (100.0 if b['state'] == 'passed' else 0.0)
        if b['state'] == 'live' and core.get('hold') == 'ready':
            b['phase'] = 'held session'  # Start-TestSession.ps1: steps from <run>\inbox until Stop-TestSession.ps1
        b['ok'] = b['state'] == 'passed' and not ((b.get('verdict') or {}).get('issues'))
        b['root'] = self.root_label(self.roots().get(rk, ''))
        b['restore_cmd'] = ('pwsh -File tools\\testenv\\Restore-TestRun.ps1 -RunDir "%s"' % run_dir) \
            if b['needs_restore'] else None
        b.pop('needs_restore_files', None)
        b.pop('game_dir', None)
        return b

    def all_names(self, root_filter=None):
        out = []
        for rk, root in self.roots().items():
            if root_filter and rk != root_filter:
                continue
            for n in self.names(rk, root):
                out.append((rk, n))
        out.sort(key=lambda x: (NAME.match(x[1]).group(1) if NAME.match(x[1]) else '00000000-000000', x[1]),
                 reverse=True)
        return out

    def list_runs(self, limit=50, before=None, state=None, scenario=None, root=None):
        sup = set(self.procs.pids('st_testenv_supervisor.exe'))
        states = set(state.split(',')) if state else None
        out, nxt, started = [], None, before is None
        for rk, n in self.all_names(root):
            ident = rk + '/' + n
            if not started:
                started = ident == before
                continue
            if scenario and scenario.lower() not in n.lower():
                continue
            b = self.brief(rk, n, sup_pids=sup)
            if not b:
                continue
            if states:
                st = b['state']
                group = {'passed': 'passed', 'failed': 'failed', 'violation': 'failed', 'interrupted': 'failed',
                         'aborted-before-start': 'failed', 'refused': 'refused', 'live': 'live',
                         'starting': 'live'}.get(st, st)
                if st not in states and group not in states:
                    continue
            if len(out) >= limit:
                nxt = out[-1]['rootkey'] + '/' + out[-1]['name']
                break
            out.append(b)
        return {'runs': out, 'next': nxt,
                'roots': [{'key': k, 'path': self.root_label(v)} for k, v in self.roots().items()]}

    def counts_today(self):
        sup = set(self.procs.pids('st_testenv_supervisor.exe'))
        pre = today_prefix()
        c = {'passed': 0, 'failed': 0, 'refused': 0, 'interrupted': 0, 'live': 0, 'total': 0}
        restore = []
        for rk, n in self.all_names():
            if not n.startswith(pre):
                continue
            b = self.brief(rk, n, sup_pids=sup)
            if not b:
                continue
            c['total'] += 1
            st = b['state']
            if st == 'passed':
                c['passed'] += 1
            elif st in ('failed', 'violation', 'aborted-before-start'):
                c['failed'] += 1
            elif st in ('live', 'starting'):
                c['live'] += 1
            elif st in c:
                c[st] += 1
            if b['needs_restore']:
                restore.append(rk + '/' + n)
        c['needs_restore'] = restore
        return c

    # ---- live runs
    def live_runs(self, sups):
        """Briefs of the runs whose supervisor is alive (normally at most one: the supervisor's mutex)."""
        out = []
        sup_pids = {s['pid'] for s in sups}
        seen = set()
        for s in sups:
            if s.get('run_dir') and s.get('mode') != 'restore' and not self._is_ctest(s['run_dir']):
                self.remember_root(os.path.dirname(os.path.abspath(s['run_dir'])))
        for rk, n in self.all_names()[:40]:
            b = self.brief(rk, n, sup_pids=sup_pids)
            if b and b['state'] in ('live', 'starting'):
                seen.add((rk, n))
                out.append(self.live_view(rk, n, b))
        self._prune_cache(seen)
        return out

    def _prune_cache(self, live):
        """Screenshots copied from a live run are in the run folder once it ends: drop their cache (once a minute)."""
        if not self.state_dir or time.time() - getattr(self, '_pruned', 0) < 60:
            return
        self._pruned = time.time()
        base = os.path.join(self.state_dir, 'cache')
        try:
            for rk in os.listdir(base):
                for n in os.listdir(os.path.join(base, rk)):
                    if (rk, n) not in live:
                        shutil.rmtree(os.path.join(base, rk, n), ignore_errors=True)
        except OSError:
            pass

    @staticmethod
    def _is_ctest(run_dir):
        return '\\testenv-work\\' in run_dir.lower().replace('/', '\\') or 'testenv-work' in run_dir.lower()

    def _game_paths(self, game_dir):
        """Live paths in the game folder, as the supervisor deploys them (supervisor.cpp)."""
        if not game_dir or not os.path.isfile(os.path.join(game_dir, 'ds.exe')):
            return None
        st = os.path.join(game_dir, 'stranded_together')
        g = os.path.join(st, 'automation', 'guest')
        return {'host': {'log': os.path.join(st, 'stranded_together.log'), 'out': os.path.join(st, 'automation', 'out')},
                'guest': {'log': os.path.join(g, 'stranded_together.log'), 'out': os.path.join(g, 'automation', 'out')}}

    def _cached(self, key, ttl, fn):
        now = time.time()
        with self._lock:
            c = self._live_cache.get(key)
            if c and now - c[0] < ttl:
                return c[1]
        v = fn()
        with self._lock:
            self._live_cache[key] = (now, v)
        return v

    def _game_dir_of(self, rk, name, run_dir):
        segs = self._segments(rk, name, run_dir) or []
        run = next((s for s in segs if s['kind'] == 'run'), None)
        return (run or {}).get('plan', {}) and run['plan'].get('game_dir')

    def live_view(self, rk, name, b):
        run_dir = self.resolve(rk, name)
        v = dict(b)
        v['games'] = {}
        v['live_shots'] = []
        if b['state'] != 'live' or not run_dir:
            return v
        gp = self._game_paths(self._game_dir_of(rk, name, run_dir)) if b.get('game_live') else None
        sides = ['host', 'guest'] if b.get('two_games') else ['host']
        if gp:
            for side in sides:
                stp = os.path.join(gp[side]['out'], 'status.txt')
                # status.txt is rewritten (.tmp + rename) every second: read at most every 2 s.
                status = self._cached(('status', stp), 2.0, lambda p=stp: parse_status_txt(read_text(p, 64 * 1024)))
                ready = self._cached(('ready', side, run_dir), 5.0,
                                     lambda p=os.path.join(gp[side]['out'], 'ready.txt'): parse_status_txt(read_text(p, 4096)))
                keep = ('uptime_s', 'fps', 'presents', 'backbuffer', 'commands', 'failures', 'pad_connected',
                        'saves_redirected', 'snapshot', 'menu_open')
                v['games'][side] = {'status': {k: status[k] for k in keep if k in status},
                                    'build': ready.get('build'), 'pid': ready.get('pid')}
                for (n, size, mt) in self._cached(('pngs', gp[side]['out']), 2.0,
                                                   lambda d=gp[side]['out']: _list_pngs(d)):
                    v['live_shots'].append({'side': side, 'name': n, 'size': size, 'mtime': mt,
                                            'url': '/api/runs/%s/%s/shot/%s/%s?v=%d_%d' % (rk, name, side, n, mt, size)})
        v['live_shots'].sort(key=lambda s: s['mtime'])
        last = b.get('last_step')
        nxt = scenario_line(run_dir, last['line'] if last else 0) if b.get('phase') in ('steps', 'booting') else None
        v['next_step'] = nxt if last else None
        v['current'] = ('line %d: %s' % (nxt['line'], nxt['what'])) if (nxt and last) else \
            {'starting': 'starting the supervisor', 'preparing': 'backing up saves and game folder, deploying',
             'booting': 'booting the game(s)', 'finishing': 'scenario done, quitting the games',
             'collecting': 'collecting logs and screenshots', 'restoring': 'restoring the game folder',
             'checking': 'checking saves and Steam Cloud'}.get(b.get('phase') or 'starting', b.get('phase'))
        tail = self.log_lines(rk, name, 'supervisor', None, 12)
        v['sup_tail'] = (tail or {}).get('lines', [])
        return v

    # ---- detail, logs, shots
    def detail(self, rk, name):
        run_dir = self.resolve(rk, name)
        if not run_dir:
            return None
        b = self.brief(rk, name, run_dir)
        segs = self._segments(rk, name, run_dir) or []
        run = next((s for s in segs if s['kind'] == 'run'), None)
        d = dict(b)
        d['steps'] = [dict(s, detail=clip(s['detail'] or '', 2000)) for s in (run or {}).get('steps', [])]
        d['phase_times'] = (run or {}).get('phase_times', {})
        d['segments'] = [{'kind': s['kind'], 'pid': s['pid'], 'final': s['final'], 'records': len(s['records'])}
                         for s in segs]
        d['plan'] = {k: v for k, v in ((run or {}).get('plan') or {}).items() if k != 'game_dir'}
        d['shots_list'] = self.shots(rk, name, run_dir, b)
        files = {}
        for which in LOG_FILES:
            p = self._log_path(rk, name, run_dir, b, which)
            files[which] = bool(p and os.path.exists(p[0]))
        d['files'] = files
        summ = read_json(os.path.join(run_dir, 'summary.json')) or {}
        d['resources'] = summ.get('resources')
        d['summary_extra'] = {k: summ.get(k) for k in ('violation_text', 'saves_refused', 'exit_trace', 'end_reason',
                                                      'restore_notes', 'saves_restore_notes', 'save_changes',
                                                      'scratch_changes') if summ.get(k)}
        if b['state'] in ('interrupted',):
            d['note'] = 'Interrupted: the restore removes the game folder\'s automation folder, so this run\'s game ' \
                        'logs and screenshots are gone.'
        return d

    def shots(self, rk, name, run_dir=None, b=None):
        run_dir = run_dir or self.resolve(rk, name)
        if not run_dir:
            return []
        b = b or self.brief(rk, name, run_dir)
        out = []
        if b and b.get('game_live'):
            return self.live_view(rk, name, b).get('live_shots', [])
        for side, sub in (('host', 'automation'), ('guest', os.path.join('guest', 'automation'))):
            for n, size, mt in _list_pngs(os.path.join(run_dir, sub)):
                out.append({'side': side, 'name': n, 'size': size, 'mtime': mt,
                            'url': '/api/runs/%s/%s/shot/%s/%s?v=%d_%d' % (rk, name, side, n, mt, size)})
        out.sort(key=lambda s: (s['side'] != 'host', s['mtime'], s['name']))  # the host's, then the guest's
        return out

    def shot_bytes(self, rk, name, side, fname):
        """PNG bytes. A finished run serves from its folder; a live run from devdash's cache, filled by
        reading each new (name, size, mtime) from the game folder once (a held PNG would make a re-shot
        'screenshot' step fail)."""
        if side not in ('host', 'guest') or not SAFE_PNG.match(fname or ''):
            return None, False
        run_dir = self.resolve(rk, name)
        if not run_dir:
            return None, False
        sub = 'automation' if side == 'host' else os.path.join('guest', 'automation')
        p = os.path.join(run_dir, sub, fname)
        if os.path.isfile(p):
            return read_bytes(p), True
        b = self.brief(rk, name, run_dir)
        if not b or not b.get('game_live'):
            return None, False
        gp = self._game_paths(self._game_dir_of(rk, name, run_dir))
        if not gp:
            return None, False
        src = os.path.join(gp[side]['out'], fname)
        try:
            st = os.stat(src)
        except OSError:
            return None, False
        cdir = os.path.join(self.state_dir, 'cache', rk, name, side)
        cp = os.path.join(cdir, '%s.%d_%d' % (fname, st.st_size, int(st.st_mtime)))
        if os.path.isfile(cp):
            return read_bytes(cp), False
        data = read_bytes(src)
        if not data or len(data) != st.st_size:
            return None, False
        try:
            os.makedirs(cdir, exist_ok=True)
            with open(cp, 'wb') as f:
                f.write(data)
        except OSError:
            pass
        return data, False

    def _log_path(self, rk, name, run_dir, b, which):
        """(path, source) for a log; game logs come from the game folder only while the games run (game_live)."""
        rel = LOG_FILES.get(which)
        if rel is None:
            return None
        live = b and b.get('game_live')
        if live and which in LIVE_LOGS:
            gp = self._game_paths(self._game_dir_of(rk, name, run_dir))
            if gp:
                side, kind = LIVE_LOGS[which]
                p = gp[side]['log'] if kind == 'log' else os.path.join(gp[side]['out'], 'results.txt')
                return p, 'game-folder'
            return None
        return os.path.join(run_dir, rel), 'run'

    def log_lines(self, rk, name, which, after=None, tail=300, source=None):
        from dd_util import read_lines_from
        run_dir = self.resolve(rk, name)
        if not run_dir:
            return None
        b = self.brief(rk, name, run_dir) if which != 'supervisor' else {'state': 'n/a'}
        pp = self._log_path(rk, name, run_dir, b, which)
        if not pp:
            return {'which': which, 'source': None, 'offset': 0, 'size': 0, 'reset': True, 'lines': [],
                    'missing': True}
        path, src = pp
        if source and source != src:
            after = None
        r = read_lines_from(path, after, tail)
        if r is None:
            return {'which': which, 'source': src, 'offset': 0, 'size': 0, 'reset': True, 'lines': [],
                    'missing': True}
        r.update({'which': which, 'source': src})
        return r


LOG_FILES = OrderedDict([
    ('supervisor', 'supervisor.log'),
    ('host', os.path.join('game', 'stranded_together.log')),
    ('guest', os.path.join('guest', 'game', 'stranded_together.log')),
    ('results', os.path.join('automation', 'results.txt')),
    ('guest_results', os.path.join('guest', 'automation', 'results.txt')),
    ('host_stdout', 'game-stdout.txt'),
    ('guest_stdout', 'guest-stdout.txt'),
    ('scenario', 'scenario.txt'),
])
LIVE_LOGS = {'host': ('host', 'log'), 'guest': ('guest', 'log'), 'results': ('host', 'results'),
             'guest_results': ('guest', 'results')}
