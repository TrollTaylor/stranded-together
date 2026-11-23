"""voicecut: a local voice-line editor for Stranded Together (docs/voicecut.md). Browse Fragile's and Sam's voice
lines from the installed Death Stranding Director's Cut, cut Fragile clips into categories, compare them with Sam's,
and export them for the mod.

  python tools\\voicecut\\voicecut.py [--open] [--port 8775] [--idle-exit 900] [--game DIR] [--dir DIR]
         [--export-dir DIR] [--language english]

Python 3 standard library only. Binds 127.0.0.1 only and answers only Host 127.0.0.1/localhost. A write (POST) needs
the X-Voicecut header and a JSON body, so another web page cannot make one. Reads the game folder only (its packfiles,
ds.exe for Wwise's codebook library, oo2core to decompress); writes only to the workspace
(%LOCALAPPDATA%\\Stranded Together\\VoiceCut) and the export folder (%LOCALAPPDATA%\\Stranded Together\\Voices).
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import threading
import time
import traceback
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import vc_audio as AU  # noqa: E402
import vc_catalog as CAT  # noqa: E402
import vc_game as G  # noqa: E402
import vc_store as ST  # noqa: E402

APP = 'voicecut'
VERSION = 1
DEFAULT_PORT = 8775
PORT_TRIES = 10
STATIC = os.path.join(HERE, 'static')
STATIC_FILES = {'/': ('index.html', 'text/html; charset=utf-8'),
                '/index.html': ('index.html', 'text/html; charset=utf-8'),
                '/app.js': ('app.js', 'text/javascript; charset=utf-8'),
                '/app.css': ('app.css', 'text/css; charset=utf-8'),
                '/favicon.svg': ('favicon.svg', 'image/svg+xml')}
CSP = ("default-src 'self'; img-src 'self' data:; media-src 'self' blob:; style-src 'self' 'unsafe-inline'; "
       "script-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")
MAX_BODY = 256 * 1024


class GameSource:
    """Decodes catalog items from the installed game (the real thing; the tests use a fake with the same calls)."""

    def __init__(self, game_dir, cache_dir):
        self.game_dir = game_dir
        self.gd = G.GameData(game_dir, cache_dir)
        self._lib = None
        self._lock = threading.Lock()

    def stamp(self):
        return G.game_stamp(self.game_dir)

    def library(self):
        with self._lock:
            if self._lib is None:
                import vc_codebooks
                with open(os.path.join(self.game_dir, 'ds.exe'), 'rb') as f:
                    self._lib = vc_codebooks.load_library(f.read())
            return self._lib

    def build_catalog(self, language, progress):
        return CAT.build_catalog(self.gd, language, progress)

    def decode(self, item, progress=None):
        import vc_vorbis as V
        data = self.gd.stream(item['stream'])
        w = V.parse_wem(data)
        w, pcm = V.decode_wem(data, self.library(), want=V.voice_channels(w), progress=progress)
        return w.rate, V.downmix(w, pcm)


class App:
    """Everything the API serves; no HTTP in here (the unit tests use it directly)."""

    def __init__(self, ws, source=None, language='english', game_dir=None, now=time.time):
        self.ws = ws
        self.source = source
        self.language = language
        self.game_dir = game_dir
        self.now = now
        self.catalog = None
        self.items = {}
        self.cat_state = {'state': 'idle', 'message': '', 'done': None, 'total': None, 'error': None}
        self.decoding = {}
        self._decode_locks = {}
        self._lock = threading.Lock()
        self._measure_thread = None
        self._measure_queue = []

    # ---- catalog
    def start_catalog(self, rebuild=False):
        if self.source is None:
            self.cat_state = {'state': 'error', 'message': '', 'done': None, 'total': None,
                              'error': 'Death Stranding Director\'s Cut was not found. Start voicecut with '
                                       '--game "<the game folder>" (the folder that has ds.exe).'}
            return
        if not rebuild:
            c = CAT.load_catalog(self.ws.catalog_path, self.source.stamp(), self.language)
            if c:
                self._set_catalog(c)
                return
        if self.cat_state['state'] == 'building':
            return
        self.cat_state = {'state': 'building', 'message': 'Starting', 'done': None, 'total': None, 'error': None}

        def run():
            def progress(msg, done=None, total=None):
                self.cat_state.update({'message': msg, 'done': done, 'total': total})
            try:
                c = self.source.build_catalog(self.language, progress)
                CAT.save_catalog(self.ws.catalog_path, c)
                self._set_catalog(c)
            except BaseException as e:  # ds_archive raises SystemExit for a missing oo2core: report it, keep serving
                self.cat_state = {'state': 'error', 'message': '', 'done': None, 'total': None,
                                  'error': 'Reading the game failed: %s' % (e or type(e).__name__)}
                self.log(traceback.format_exc())
        threading.Thread(target=run, daemon=True).start()

    def _set_catalog(self, c):
        self.catalog = c
        self.items = {i['id']: i for i in c['items']}
        self.cat_state = {'state': 'ready', 'message': '%d lines' % len(c['items']), 'done': None, 'total': None,
                          'error': None}

    def log(self, text):
        try:
            with open(os.path.join(self.ws.root, 'voicecut.log'), 'a', encoding='utf-8') as f:
                f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), text))
        except OSError:
            pass

    # ---- source audio (decoded once, then cached as a WAV)
    def source_pcm(self, item_id):
        item = self.items.get(item_id)
        if item is None:
            raise KeyError(item_id)
        path = self.ws.audio_path(item_id)
        with self._lock:
            lock = self._decode_locks.setdefault(item_id, threading.Lock())
        with lock:
            if os.path.exists(path):
                try:
                    os.utime(path, None)
                    return AU.read_wav(path)
                except (OSError, EOFError, ValueError):
                    pass
            self.decoding[item_id] = 0.0
            try:
                rate, pcm = self.source.decode(item, lambda f: self.decoding.__setitem__(item_id, round(f, 3)))
            finally:
                self.decoding.pop(item_id, None)
            tmp = path + '.tmp'
            AU.write_wav(tmp, pcm, rate)
            os.replace(tmp, path)
            self.ws.prune_cache(keep=[path])
            self._remember(item_id, AU.stats(pcm, rate))
            return rate, AU.read_wav(path)[1]

    def source_wav(self, item_id):
        self.source_pcm(item_id)
        with open(self.ws.audio_path(item_id), 'rb') as f:
            return f.read()

    def _remember(self, item_id, st):
        with self._lock:
            self.ws.measured[item_id] = st
            self.ws.save_measured()

    def measure(self, ids):
        """Queue sources to decode and measure in the background (Sam's lines for the comparison)."""
        ids = [i for i in ids if i in self.items and i not in self.ws.measured]
        with self._lock:
            self._measure_queue.extend(i for i in ids if i not in self._measure_queue)
            if self._measure_thread and self._measure_thread.is_alive():
                return len(self._measure_queue)

            def run():
                while True:
                    with self._lock:
                        if not self._measure_queue:
                            return
                        i = self._measure_queue.pop(0)
                    try:
                        self.source_pcm(i)
                    except Exception:
                        self.log('measure %s failed\n%s' % (i, traceback.format_exc()))
            self._measure_thread = threading.Thread(target=run, daemon=True)
            self._measure_thread.start()
        return len(self._measure_queue)

    # ---- API
    def status(self):
        clips = self.ws.clips()
        return {'app': APP, 'version': VERSION, 'catalog': self.cat_state, 'game_dir': self.game_dir,
                'workspace': self.ws.root, 'export_dir': self.ws.export_dir, 'language': self.language,
                'clips': len(clips), 'decoding': dict(self.decoding), 'measuring': len(self._measure_queue)}

    def catalog_view(self):
        if not self.catalog:
            return {'ready': False}
        keep = ('id', 'kind', 'speaker', 'text', 'length', 'group', 'situations', 'category', 'sure', 'name', 'lines')
        return {'ready': True, 'items': [{k: i[k] for k in keep if k in i} for i in self.catalog['items']],
                'situations': self.catalog['situations'], 'categories': self.catalog['categories'],
                'built': self.catalog.get('built'), 'seconds': self.catalog.get('seconds')}

    def user_view(self):
        return {'clips': self.ws.clips(), 'situations': self.ws.state.get('situations', {}),
                'measured': self.ws.measured}

    def save_clip(self, body):
        c = ST.clean_clip(body, set(self.items) if self.items else None)
        rate, src = self.source_pcm(c['source'])
        pcm = AU.render(src, rate, c)
        if not pcm:
            raise ST.ClipError('the selection is outside the line')
        if c['end'] > len(src) / rate + 0.001:
            raise ST.ClipError('the selection ends after the line')
        return self.ws.upsert_clip(c, set(self.items) if self.items else None, AU.stats(pcm, rate))

    def situation_rows(self):
        """(hash, category, user|known|guess, label) of Sam's situations: the user's sorting, else the guess."""
        rows = []
        user = self.ws.state.get('situations', {})
        for s in (self.catalog or {}).get('situations', []):
            u = user.get(str(s['hash']))
            how = 'user' if u else ('known' if s.get('known') else 'guess')
            rows.append((s['hash'], u or s.get('guess', 'other'), how, s.get('label', '')))
        return rows

    def export(self):
        stamp = self.source.stamp() if self.source else ''
        return self.ws.export(self.source_pcm, stamp, self.language, situations=self.situation_rows())

    def open_folder(self, what):
        path = {'export': self.ws.export_dir, 'workspace': self.ws.root}.get(what)
        if not path:
            raise ST.ClipError('unknown folder')
        os.makedirs(path, exist_ok=True)
        if sys.platform == 'win32':
            os.startfile(path)  # noqa: S606 (a fixed folder of ours, opened in Explorer)
        return {'opened': path}

    def route(self, method, path, query, body=None):
        if method == 'GET':
            if path == '/api/ping':
                return 200, {'app': APP, 'version': VERSION, 'workspace': self.ws.root}
            if path == '/api/status':
                return 200, self.status()
            if path == '/api/catalog':
                return 200, self.catalog_view()
            if path == '/api/user':
                return 200, self.user_view()
            if path == '/api/audio':
                item_id = (query.get('id') or [''])[0]
                if item_id not in self.items:
                    return 404, {'error': 'unknown line'}
                return 200, self.source_wav(item_id), 'audio/wav', 3600
            return 404, {'error': 'not found'}
        if method == 'POST':
            body = body if isinstance(body, dict) else {}
            try:
                if path == '/api/clip':
                    return 200, self.save_clip(body)
                if path == '/api/clip/delete':
                    self.ws.delete_clip(str(body.get('id', '')))
                    return 200, {'deleted': body.get('id')}
                if path == '/api/situation':
                    self.ws.set_situation(body.get('hash', -1), body.get('category'))
                    return 200, {'situations': self.ws.state.get('situations', {})}
                if path == '/api/measure':
                    ids = body.get('ids') or []
                    if not isinstance(ids, list):
                        return 400, {'error': 'ids is a list'}
                    return 200, {'queued': self.measure([str(i) for i in ids[:5000]])}
                if path == '/api/export':
                    return 200, self.export()
                if path == '/api/open':
                    return 200, self.open_folder(str(body.get('what', '')))
                if path == '/api/rebuild':
                    self.start_catalog(rebuild=True)
                    return 200, {'catalog': self.cat_state}
            except ST.ClipError as e:
                return 400, {'error': str(e)}
            except KeyError:
                return 404, {'error': 'unknown line'}
            return 404, {'error': 'not found'}
        return 405, {'error': 'method'}


class Handler(BaseHTTPRequestHandler):
    server_version = 'voicecut'
    sys_version = ''
    app = None
    port = DEFAULT_PORT
    last_request = [time.time()]

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
        self.send_header('Cache-Control', 'private, max-age=%d' % cache if cache else 'no-store')
        if etag:
            self.send_header('ETag', etag)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self._handle('GET')

    def do_POST(self):
        self._handle('POST')

    def _reject(self):
        self.send_response(405)
        self.send_header('Allow', 'GET, HEAD, POST')
        self.send_header('Content-Length', '0')
        self.end_headers()

    do_PUT = do_DELETE = do_PATCH = do_OPTIONS = _reject

    def _handle(self, method):
        Handler.last_request[0] = time.time()
        if not self._host_ok():
            return self._send(403, b'{"error":"host"}')
        u = urllib.parse.urlsplit(self.path)
        path = u.path
        body = None
        if method == 'GET' and path in STATIC_FILES:
            fname, ctype = STATIC_FILES[path]
            try:
                with open(os.path.join(STATIC, fname), 'rb') as f:
                    return self._send(200, f.read(), ctype)
            except OSError:
                return self._send(404, b'{"error":"static"}')
        if not path.startswith('/api/'):
            return self._send(404, b'{"error":"not found"}')
        if method == 'POST':
            # no cross-site writes: a page elsewhere cannot set this header without a preflight, which is refused
            if self.headers.get('X-Voicecut') != '1' or \
                    not (self.headers.get('Content-Type') or '').startswith('application/json'):
                return self._send(403, b'{"error":"missing X-Voicecut header"}')
            try:
                n = int(self.headers.get('Content-Length') or 0)
            except ValueError:
                n = -1
            if not 0 <= n <= MAX_BODY:
                return self._send(413, b'{"error":"body too large"}')
            try:
                body = json.loads(self.rfile.read(n).decode('utf-8') or '{}')
            except (ValueError, UnicodeDecodeError):
                return self._send(400, b'{"error":"bad json"}')
        try:
            r = self.app.route(method, path, urllib.parse.parse_qs(u.query), body)
        except Exception:  # keep serving; the page shows the error
            self.app.log('%s %s\n%s' % (method, self.path, traceback.format_exc()))
            return self._send(500, b'{"error":"internal error (see voicecut.log in the workspace)"}')
        if len(r) == 4:
            code, data, ctype, cache = r
            return self._send(code, data, ctype, cache)
        code, obj = r
        out = json.dumps(obj, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
        etag = '"%s"' % hashlib.sha1(out).hexdigest()[:16]
        if method == 'GET' and code == 200 and self.headers.get('If-None-Match') == etag:
            self.send_response(304)
            self.send_header('ETag', etag)
            self.end_headers()
            return
        self._send(code, out, etag=etag if method == 'GET' else None)


class Server(ThreadingHTTPServer):
    allow_reuse_address = False  # SO_REUSEADDR on Windows would let two servers share a port
    daemon_threads = True

    def handle_error(self, request, client_address):
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionError, TimeoutError)):
            return
        if Handler.app:
            Handler.app.log('server error\n' + traceback.format_exc())

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
            subprocess.Popen([exe, '--app=' + url, '--window-size=1500,920'], close_fds=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
            return
        except OSError:
            pass
    import webbrowser
    webbrowser.open(url)


def startup_error(state, msg, no_console):
    """A failure before the server runs: printed, logged and (without a console) shown in a message box."""
    if not no_console:
        print('voicecut: ' + msg)
    where = None
    try:
        os.makedirs(state, exist_ok=True)
        where = os.path.join(state, 'voicecut.log')
        with open(where, 'a', encoding='utf-8') as f:
            f.write('%s startup: %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
    except OSError:
        pass
    if no_console and sys.platform == 'win32':
        try:
            import ctypes
            text = 'voicecut could not start:\n\n' + msg + ('\n\n(also in %s)' % where if where else '')
            ctypes.windll.user32.MessageBoxW(None, text, 'voicecut', 0x10)
        except Exception:
            pass


def serve(args):
    no_console = sys.stdout is None
    if sys.stdout is None:  # pythonw / pyw: no console
        sys.stdout = open(os.devnull, 'w')
    if sys.stderr is None:
        sys.stderr = open(os.devnull, 'w')
    root = args.dir or ST.default_workspace()
    try:
        ws = ST.Workspace(root, args.export_dir)
    except OSError as e:
        startup_error(root, 'cannot create the workspace %s: %s' % (root, e), no_console)
        return 1
    server = None
    port = None
    for p in range(args.port, args.port + PORT_TRIES):
        j = ping(p)
        if j and os.path.normcase(j.get('workspace') or '') == os.path.normcase(ws.root):
            if args.open:
                open_ui('http://127.0.0.1:%d/' % p, not args.tab)
            print('voicecut already running: http://127.0.0.1:%d/' % p)
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
        startup_error(root, 'no free port in %d..%d on 127.0.0.1' % (args.port, args.port + PORT_TRIES - 1),
                      no_console)
        return 1
    game_dir = G.find_game_dir(args.game)
    source = None
    if game_dir:
        try:
            source = GameSource(game_dir, os.path.join(ws.root, 'index'))
        except Exception as e:
            ws_log = 'opening the game failed: %s' % e
            startup_error(root, ws_log, True) if no_console else print(ws_log)
    app = App(ws, source, args.language, game_dir)
    Handler.app = app
    Handler.port = port
    app.start_catalog()
    url = 'http://127.0.0.1:%d/' % port
    print('voicecut: %s (Ctrl+C to stop; exits after %d s without a request)' % (url, args.idle_exit))
    sys.stdout.flush()

    def idle_watch():
        while True:
            time.sleep(10)
            busy = app.decoding or app.cat_state['state'] == 'building'
            if args.idle_exit and not busy and time.time() - Handler.last_request[0] > args.idle_exit:
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


def main(argv=None):
    ap = argparse.ArgumentParser(prog='voicecut', description=__doc__.split('\n')[0])
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--open', action='store_true', help='open the page (an Edge/Chrome app window)')
    ap.add_argument('--tab', action='store_true', help='with --open: the default browser instead of an app window')
    ap.add_argument('--idle-exit', type=int, default=900, help='exit after N s without a request (0: never)')
    ap.add_argument('--game', help='the game folder (default: found through Steam)')
    ap.add_argument('--dir', help='the workspace (default %%LOCALAPPDATA%%\\Stranded Together\\VoiceCut)')
    ap.add_argument('--export-dir', help='default %%LOCALAPPDATA%%\\Stranded Together\\Voices')
    ap.add_argument('--language', default='english', choices=G.TEXT_LANGUAGES)
    return serve(ap.parse_args(argv))


if __name__ == '__main__':
    sys.exit(main())
