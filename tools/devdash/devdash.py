"""devdash: a local, read-only dashboard of hidden test runs, batches, posted tasks and Claude
workflow runs (docs/devdash.md). A separate program: it never starts, stops or touches the game.

  python tools\\devdash\\devdash.py [--open] [--port 8765] [--idle-exit 900]
  python tools\\devdash\\devdash.py post <id> [--title T] [--kind test|batch|research|build|other]
         [--total N] [--done N] [--status S] [--log MSG] [--level info|warn|error]
         [--item KEY --item-status queued|running|pass|fail|skip --run-dir DIR --detail D] [--close STATUS]

Python 3 standard library only. Binds 127.0.0.1 only; GET only; answers only Host 127.0.0.1/localhost.
Writes only under its state folder (<repo>\\build-devdash, gitignored; ST_DEVDASH_DIR overrides).
"""
import argparse
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer  # noqa: F401

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from dd_flows import AGENTID, RUNID, FlowStore  # noqa: E402
from dd_proc import Procs  # noqa: E402
from dd_runs import ROOTKEY, SAFE_NAME, SAFE_PNG, RunStore  # noqa: E402
from dd_tasks import FINAL_STATES, KINDS, TASK_ID, TaskStore, post  # noqa: E402
from dd_util import Redactor, mtime_ms, now_ms, read_text  # noqa: E402

APP = 'devdash'
VERSION = 1
DEFAULT_PORT = 8765
PORT_TRIES = 10
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
STATIC = os.path.join(HERE, 'static')
STATIC_FILES = {'/': ('index.html', 'text/html; charset=utf-8'),
                '/index.html': ('index.html', 'text/html; charset=utf-8'),
                '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                '/app.css': ('app.css', 'text/css; charset=utf-8'),
                '/favicon.svg': ('favicon.svg', 'image/svg+xml')}
CSP = ("default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self'; "
       "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")


def default_state_dir(repo=REPO):
    return os.environ.get('ST_DEVDASH_DIR') or os.path.join(repo, 'build-devdash')


def repo_id(repo):
    return hashlib.sha1(os.path.normcase(repo).encode('utf-8')).hexdigest()[:12]


class Dash:
    """Everything the API serves; no HTTP in here (the unit tests use it directly)."""

    def __init__(self, repo=REPO, state_dir=None, projects_dir=None, procs=None, extra_roots=(), now=None):
        self.repo = repo
        self.state_dir = state_dir or default_state_dir(repo)
        self.procs = procs or Procs()
        self.runs = RunStore(repo, self.state_dir, self.procs, extra_roots, now=now)
        self.flows = FlowStore(repo, projects_dir, now=now)
        self.tasks = TaskStore(self.state_dir, self.procs, now=now)
        self.redact = Redactor()
        self.started = now_ms()
        self._ctest = {}

    # ---- ctest (LastTest.log per build folder)
    @staticmethod
    def _ctest_log(tmp_dir):
        """The log of the newest ctest in a Testing/Temporary folder, and its mtime. A running ctest writes
        LastTest.log.tmp<random> and renames it to LastTest.log only when it ends (LastTest.log is then still the
        previous run's): the newest .tmp wins while it is newer than LastTest.log."""
        p = os.path.join(tmp_dir, 'LastTest.log')
        best, best_mt = p, mtime_ms(p)
        try:
            names = [n for n in os.listdir(tmp_dir) if n.startswith('LastTest.log.tmp')]
        except OSError:
            names = []
        for n in names:
            q = os.path.join(tmp_dir, n)
            mt = mtime_ms(q)
            if mt is not None and (best_mt is None or mt > best_mt):
                best, best_mt = q, mt
        return best, best_mt

    def ctest(self):
        out = []
        running = bool(self.procs.pids('ctest.exe'))
        try:
            dirs = [d for d in os.listdir(self.repo) if d.lower().startswith('build')]
        except OSError:
            dirs = []
        for d in sorted(dirs):
            p, mt = self._ctest_log(os.path.join(self.repo, d, 'Testing', 'Temporary'))
            if mt is None or time.time() * 1000 - mt > 12 * 3600 * 1000:
                continue
            t = read_text(p, 512 * 1024) or ''
            k = n = 0
            cur, failed, ended = None, [], 'End testing' in t
            for ln in t.split('\n'):
                m = re.match(r'^(\d+)/(\d+) Testing: (\S+)', ln)
                if m:
                    k, n, cur = int(m.group(1)), int(m.group(2)), m.group(3)
                elif ln.startswith('Test Failed') or ln.startswith('***') and cur:
                    failed.append(cur)
            done = k if ended else max(0, k - 1)
            out.append({'build': d, 'state': 'done' if ended else ('running' if running else 'stopped'),
                        'k': done, 'n': n, 'current': None if ended else cur, 'failed': sorted(set(failed)),
                        'updated': mt})
        return out

    # ---- batches: link posted items to runs
    def _link_items(self, task, live):
        # task['items'] are copies (TaskStore.brief): 'run' is set fresh on every call, never left from an earlier one
        for it in task.get('items', []):
            it.pop('run', None)
            ref = None
            if it.get('run_dir'):
                ref = self.runs.find_run_dir(it['run_dir'])
            elif it['status'] == 'running':
                scen = it.get('scenario') or it['key']
                for lv in live:
                    if lv['scenario'] == scen and (lv.get('stamp') or 0) >= (it.get('started') or 0) - 120000:
                        ref = (lv['rootkey'], lv['name'])
            if ref:
                b = self.runs.brief(*ref)
                if b:
                    it['run'] = {'rootkey': b['rootkey'], 'name': b['name'], 'state': b['state'], 'pct': b['pct'],
                                 'steps_done': b.get('steps_done'), 'steps_total': b.get('steps_total'),
                                 'duration_s': b.get('duration_s'), 'ok': b['ok'],
                                 'issues': (b.get('verdict') or {}).get('issues', [])}
        return task

    def overview(self):
        sups = self.procs.supervisors()
        live = self.runs.live_runs(sups)
        live_pids = {lv.get('pid') for lv in live}
        other = []
        for s in sups:
            if s['pid'] in live_pids:
                continue
            rd = s.get('run_dir') or ''
            kind = 'ctest-e2e' if 'testenv-work' in rd.lower() else s.get('mode', 'run')
            other.append({'pid': s['pid'], 'kind': kind, 'run': os.path.basename(rd.rstrip('\\/')) if rd else None})
        tasks = self.tasks.list(with_items=True)
        batches, posted = [], []
        for t in tasks:
            if t['kind'] == 'batch' or t.get('items'):
                batches.append(self._link_items(t, live))
            else:
                t.pop('items', None)
                posted.append(t)
        flows = self.flows.list(limit=12)
        subs = self.flows.plain_subagents_running()
        today = self.runs.counts_today()
        queued = 0
        for b in batches:
            if b['status'] == 'running':  # a stale batch (abandoned) runs nothing more
                queued += b['items_count'].get('queued', 0)
                if b.get('total'):
                    queued += max(0, b['total'] - len(b.get('items', [])))
        counts = {
            'tests_running': len(live), 'tests_queued': queued,
            'passed_today': today['passed'], 'failed_today': today['failed'] + today['interrupted'],
            'refused_today': today['refused'], 'runs_today': today['total'],
            'needs_restore': today['needs_restore'],
            'workflows_running': sum(1 for f in flows if f['status'] == 'running'),
            'agents_running': sum(f['agents_count']['running'] for f in flows if f['status'] == 'running') + len(subs),
            'tasks_running': sum(1 for t in tasks if t['status'] == 'running'),
            'ds_running': len(self.procs.pids('ds.exe')),
        }
        return {'now': now_ms(), 'counts': counts, 'live': live, 'other_supervisors': other,
                'ctest': self.ctest(), 'batches': batches, 'tasks': posted, 'workflows': flows,
                'subagents': subs}

    # ---- routing (path, query) -> (status, body, content type, cache seconds)
    def route(self, path, q):
        def qi(name, default, lo, hi):
            try:
                return max(lo, min(hi, int(q.get(name, [default])[0])))
            except (TypeError, ValueError):
                return default

        def qs(name):
            v = q.get(name, [None])[0]
            return v if v is None else v[:200]

        if path == '/api/ping':
            return 200, {'app': APP, 'v': VERSION, 'pid': os.getpid(), 'started': self.started,
                         'repo': repo_id(self.repo)}
        if path == '/api/overview':
            return 200, self.overview()
        if path == '/api/runs':
            return 200, self.runs.list_runs(limit=qi('limit', 50, 1, 500), before=qs('before'), state=qs('state'),
                                            scenario=qs('scenario'), root=qs('root'))
        m = re.match(r'^/api/runs/([^/]+)/([^/]+)(?:/(log|shots|shot)(?:/([^/]+)/([^/]+))?)?$', path)
        if m:
            rk, name, sub, side, fname = m.groups()
            if not ROOTKEY.match(rk) or not SAFE_NAME.match(name):
                return 404, {'error': 'no such run'}
            if sub is None:
                d = self.runs.detail(rk, name)
                return (200, d) if d else (404, {'error': 'no such run'})
            if sub == 'shots':
                return 200, {'shots': self.runs.shots(rk, name)}
            if sub == 'log':
                after = q.get('after', [None])[0]
                try:
                    after = int(after) if after not in (None, '') else None
                except ValueError:
                    after = None
                which = qs('which') or 'supervisor'
                r = self.runs.log_lines(rk, name, which, after, qi('tail', 400, 1, 5000), qs('source'))
                return (200, r) if r else (404, {'error': 'no such run'})
            if sub == 'shot' and side and fname and SAFE_PNG.match(fname):
                data, final = self.runs.shot_bytes(rk, name, side, fname)
                if data:
                    return 200, data, 'image/png', (86400 if final else 30)
            return 404, {'error': 'not found'}
        if path == '/api/workflows':
            return 200, {'workflows': self.flows.list(limit=qi('limit', 30, 1, 200))}
        m = re.match(r'^/api/workflows/([^/]+)(?:/agents/([^/]+))?$', path)
        if m:
            rid, aid = m.groups()
            if not RUNID.match(rid) or (aid and not AGENTID.match(aid)):
                return 404, {'error': 'not found'}
            r = self.flows.agent_events(rid, aid, qi('tail', 60, 1, 500)) if aid else self.flows.detail(rid)
            return (200, r) if r else (404, {'error': 'not found'})
        if path == '/api/tasks':
            return 200, {'tasks': self.tasks.list(include_old=q.get('all', ['0'])[0] == '1')}
        m = re.match(r'^/api/tasks/([^/]+)$', path)
        if m and TASK_ID.match(m.group(1)):
            d = self.tasks.detail(m.group(1))
            if d:
                live = self.runs.live_runs(self.procs.supervisors())
                self._link_items(d, live)
                return 200, d
        return 404, {'error': 'not found'}


class Handler(BaseHTTPRequestHandler):
    server_version = 'devdash'
    sys_version = ''
    dash = None
    port = DEFAULT_PORT
    last_request = [time.time()]
    log_path = None

    def log_message(self, fmt, *args):  # pythonw has no stderr; requests are not logged
        pass

    def _host_ok(self):
        host = (self.headers.get('Host') or '').lower()
        return host in ('127.0.0.1:%d' % self.port, 'localhost:%d' % self.port)

    def _send(self, code, body, ctype='application/json; charset=utf-8', cache=0, etag=None):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', CSP)
        self.send_header('Cache-Control', 'max-age=%d' % cache if cache else 'no-store')
        if etag:
            self.send_header('ETag', etag)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def _reject(self):
        self.send_response(405)
        self.send_header('Allow', 'GET, HEAD')
        self.send_header('Content-Length', '0')
        self.end_headers()

    do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _reject

    def do_GET(self):
        Handler.last_request[0] = time.time()
        if not self._host_ok():
            return self._send(403, b'{"error":"host"}')
        u = urllib.parse.urlsplit(self.path)
        path = u.path
        if path in STATIC_FILES:
            fname, ctype = STATIC_FILES[path]
            try:
                with open(os.path.join(STATIC, fname), 'rb') as f:
                    return self._send(200, f.read(), ctype)
            except OSError:
                return self._send(404, b'{"error":"static"}')
        if not path.startswith('/api/'):
            return self._send(404, b'{"error":"not found"}')
        try:
            r = self.dash.route(path, urllib.parse.parse_qs(u.query))
        except Exception:  # keep serving; the page shows the error
            self._log_error(traceback.format_exc())
            return self._send(500, b'{"error":"internal error (see build-devdash/devdash.log)"}')
        if len(r) == 4:
            code, data, ctype, cache = r
            return self._send(code, data, ctype, cache)
        code, obj = r
        body = json.dumps(self.dash.redact.obj(obj), ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        etag = '"%s"' % hashlib.sha1(body).hexdigest()[:16]
        if code == 200 and self.headers.get('If-None-Match') == etag:
            self.send_response(304)
            self.send_header('ETag', etag)
            self.end_headers()
            return
        self._send(code, body, etag=etag)

    def _log_error(self, text):
        if not self.log_path:
            return
        try:
            with open(self.log_path, 'a', encoding='utf-8') as f:
                f.write('%s %s\n%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), self.path,
                                        self.dash.redact.text(text)))
        except OSError:
            pass


class Server(ThreadingHTTPServer):
    allow_reuse_address = False  # SO_REUSEADDR on Windows would let two servers share a port
    daemon_threads = True

    def handle_error(self, request, client_address):  # pythonw has no stderr: log to build-devdash/devdash.log
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionError, TimeoutError)):
            return  # the page went away mid-answer
        if Handler.log_path:
            try:
                with open(Handler.log_path, 'a', encoding='utf-8') as f:
                    f.write('%s server error\n%s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), traceback.format_exc()))
            except OSError:
                pass

    def server_bind(self):
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def ping(port, timeout=0.6):
    try:
        with urllib.request.urlopen('http://127.0.0.1:%d/api/ping' % port, timeout=timeout) as r:
            j = json.loads(r.read().decode('utf-8'))
            return j if j.get('app') == APP else None
    except (OSError, ValueError):
        return None


def find_app_browser():
    """Edge (or Chrome) for an app window (--app=URL): no tabs, no address bar."""
    cands = []
    for env in ('ProgramFiles(x86)', 'ProgramFiles', 'LOCALAPPDATA'):
        base = os.environ.get(env)
        if base:
            cands.append(os.path.join(base, 'Microsoft', 'Edge', 'Application', 'msedge.exe'))
    for env in ('ProgramFiles', 'ProgramFiles(x86)', 'LOCALAPPDATA'):
        base = os.environ.get(env)
        if base:
            cands.append(os.path.join(base, 'Google', 'Chrome', 'Application', 'chrome.exe'))
    return next((c for c in cands if os.path.isfile(c)), None)


def open_ui(url, app_window=True):
    exe = find_app_browser() if app_window else None
    if exe:
        try:
            subprocess.Popen([exe, '--app=' + url, '--window-size=1440,900'], close_fds=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
            return
        except OSError:
            pass
    import webbrowser
    webbrowser.open(url)


def startup_error(state, msg, no_console):
    """A failure before the server runs: printed, appended to <state>/devdash.log and, without a console (pythonw /
    pyw, how devdash.bat starts it), shown in a message box, so a double-click never fails silently."""
    if not no_console:
        print('devdash: ' + msg)
    try:
        os.makedirs(state, exist_ok=True)
        with open(os.path.join(state, 'devdash.log'), 'a', encoding='utf-8') as f:
            f.write('%s startup: %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
        where = os.path.join(state, 'devdash.log')
    except OSError:
        where = None
    if no_console and sys.platform == 'win32':
        try:
            import ctypes
            text = 'devdash could not start:\n\n' + msg + ('\n\n(also in %s)' % where if where else '')
            ctypes.windll.user32.MessageBoxW(None, text, 'devdash', 0x10)
        except Exception:
            pass


def serve(args):
    no_console = sys.stdout is None
    if sys.stdout is None:  # pythonw / pyw: no console
        sys.stdout = open(os.devnull, 'w')
    if sys.stderr is None:
        sys.stderr = open(os.devnull, 'w')
    state = args.state_dir or default_state_dir()
    try:
        os.makedirs(state, exist_ok=True)
    except OSError as e:
        startup_error(state, 'cannot create the state folder %s: %s' % (state, e), no_console)
        return 1
    me = repo_id(REPO)
    server = None
    port = None
    for p in range(args.port, args.port + PORT_TRIES):
        j = ping(p)
        if j and j.get('repo') == me:  # already running for this repo: just show it
            if args.open:
                open_ui('http://127.0.0.1:%d/' % p, not args.tab)
            print('devdash already running: http://127.0.0.1:%d/' % p) if sys.stdout else None
            return 0
        if j:
            continue
        try:
            server = Server(('127.0.0.1', p), Handler)
            port = p
            break
        except OSError:
            continue
    if server is None:
        startup_error(state, 'no free port in %d..%d on 127.0.0.1' % (args.port, args.port + PORT_TRIES - 1), no_console)
        return 1
    Handler.dash = Dash(REPO, state, args.claude_dir, extra_roots=args.root or ())
    Handler.port = port
    Handler.log_path = os.path.join(state, 'devdash.log')

    def warm():  # the first overview reads every workflow summary and today's runs: do it before the page asks
        try:
            Handler.dash.overview()
            Handler.dash.runs.list_runs(limit=40)
        except Exception:
            pass
    threading.Thread(target=warm, daemon=True).start()
    url = 'http://127.0.0.1:%d/' % port
    try:
        with open(os.path.join(state, 'server.json'), 'w', encoding='utf-8') as f:
            json.dump({'url': url, 'port': port, 'pid': os.getpid(), 'started': now_ms()}, f)
    except OSError:
        pass
    if sys.stdout:
        print('devdash: %s (Ctrl+C to stop; exits after %d s without a request)' % (url, args.idle_exit))
        sys.stdout.flush()

    def idle_watch():
        while True:
            time.sleep(10)
            if args.idle_exit and time.time() - Handler.last_request[0] > args.idle_exit:
                server.shutdown()
                return
    threading.Thread(target=idle_watch, daemon=True).start()
    if args.open:
        threading.Timer(0.3, open_ui, (url, not args.tab)).start()
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def cmd_post(args):
    if not TASK_ID.match(args.id):
        print('task id must match %s' % TASK_ID.pattern)
        return 2
    recs = []
    if args.title or args.kind or args.parent or args.owner:
        r = {'op': 'open', 'kind': args.kind or 'other', 'title': args.title or args.id}
        for k in ('total', 'parent', 'owner', 'pid', 'heartbeat_s'):
            if getattr(args, k) is not None:
                r[k] = getattr(args, k)
        recs.append(r)
    if args.done is not None or (args.total is not None and not recs) or \
            (args.status and args.status not in FINAL_STATES):
        r = {'op': 'progress'}
        if args.done is not None:
            r['done'] = args.done
        if args.total is not None:
            r['total'] = args.total
        if args.status and args.status not in FINAL_STATES:
            r['status'] = args.status
        recs.append(r)
    if args.item:
        r = {'op': 'item', 'key': args.item, 'status': args.item_status or 'running'}
        for k in ('run_dir', 'detail', 'scenario'):
            if getattr(args, k):
                r[k] = getattr(args, k)
        recs.append(r)
    if args.log:
        recs.append({'op': 'log', 'level': args.level, 'msg': args.log})
    close = args.close or (args.status if args.status in FINAL_STATES else None)
    if close:
        recs.append({'op': 'close', 'status': close, 'summary': args.summary} if args.summary else
                    {'op': 'close', 'status': close})
    if not recs:
        recs.append({'op': 'beat'})
    post(args.state_dir or default_state_dir(), args.id, recs)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='devdash', description=__doc__.split('\n')[0])
    ap.add_argument('--state-dir', help='default: <repo>\\build-devdash (or ST_DEVDASH_DIR)')
    sub = ap.add_subparsers(dest='cmd')
    sv = sub.add_parser('serve', help='run the dashboard (the default)')
    for p in (ap, sv):
        p.add_argument('--port', type=int, default=DEFAULT_PORT)
        p.add_argument('--open', action='store_true', help='open the page (an Edge/Chrome app window)')
        p.add_argument('--tab', action='store_true', help='with --open: the default browser instead of an app window')
        p.add_argument('--idle-exit', type=int, default=900, help='exit after N s without a request (0: never)')
        p.add_argument('--claude-dir', help='Claude Code projects folder (default %%USERPROFILE%%\\.claude\\projects)')
        p.add_argument('--root', action='append', help='an extra runs folder (a -BuildDir outside the repo)')
    pp = sub.add_parser('post', help='append a status record for a task (docs/devdash.md)')
    pp.add_argument('id')
    pp.add_argument('--title')
    pp.add_argument('--kind', choices=KINDS)
    pp.add_argument('--total', type=int)
    pp.add_argument('--done', type=int)
    pp.add_argument('--status')
    pp.add_argument('--parent')
    pp.add_argument('--owner')
    pp.add_argument('--pid', type=int)
    pp.add_argument('--heartbeat-s', type=int, dest='heartbeat_s')
    pp.add_argument('--log')
    pp.add_argument('--level', default='info', choices=('info', 'warn', 'error'))
    pp.add_argument('--item')
    pp.add_argument('--item-status', choices=('queued', 'running', 'pass', 'fail', 'skip'))
    pp.add_argument('--run-dir')
    pp.add_argument('--scenario')
    pp.add_argument('--detail')
    pp.add_argument('--close', choices=FINAL_STATES)
    pp.add_argument('--summary')
    args = ap.parse_args(argv)
    if args.cmd == 'post':
        return cmd_post(args)
    return serve(args)


if __name__ == '__main__':
    sys.exit(main())
