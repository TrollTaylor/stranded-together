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
import vc_match as M  # noqa: E402
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
        w, pcm = V.decode_wem(data, self.library(), mix=V.voice_mix(w), progress=progress)
        return w.rate, pcm[0]

    # background decoding: worker processes, each with its own archive reader and codebook library
    def prefetch_pool(self, workers):
        from concurrent.futures import ProcessPoolExecutor
        import vc_worker
        return ProcessPoolExecutor(max_workers=workers, initializer=vc_worker.init,
                                   initargs=(self.game_dir, self.gd.game.out_dir))

    @staticmethod
    def prefetch_submit(pool, item, path):
        import vc_worker
        return pool.submit(vc_worker.decode_to, {'id': item['id'], 'stream': item['stream']}, path)

    @staticmethod
    def analyse_submit(pool, kind, path, subs, item_id):
        import vc_worker
        return pool.submit(vc_worker.analyse, kind, path, subs, item_id)


class App:
    """Everything the API serves; no HTTP in here (the unit tests use it directly)."""

    def __init__(self, ws, source=None, language='english', game_dir=None, now=time.time, prefetch=False,
                 workers=None):
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
        self._measure_dirty = 0
        self._measure_saved = time.time()
        self.auto_prefetch = prefetch
        self.workers = workers or max(1, min(6, (os.cpu_count() or 2) - 2))
        self.prefetch = {'state': 'idle', 'done': 0, 'total': 0, 'failed': 0, 'workers': self.workers}
        self._prefetch_paused = threading.Event()
        self._prefetch_thread = None
        self._prefetch_pool = None
        self.analysis = {'state': 'idle', 'done': 0, 'total': 0, 'failed': 0}
        self.match = None
        self._cands = {}
        self._match_scales = None

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
        self.load_match()
        if self.match:
            self.analysis = {'state': 'done', 'done': 0, 'total': 0, 'failed': 0}
        if self.auto_prefetch:
            self.start_prefetch()

    # ---- decode everything in the background (worker processes), Fragile's cutscenes first
    def prefetch_order(self):
        rank = {('fragile', 'cutscene'): 0, ('fragile', 'line'): 1, ('sam', 'line'): 2}
        items = sorted(self.items.values(), key=lambda i: (rank.get((i['speaker'], i['kind']), 3), i['id']))
        return [i for i in items if not os.path.exists(self.ws.audio_path(i['id']))]

    def analysis_jobs(self):
        """What the matcher measures: Fragile's cutscenes (her candidate sounds) and Sam's wordless lines."""
        cuts = [i for i in self.items.values() if i['kind'] == 'cutscene' and i['speaker'] == 'fragile']
        sam = [i for i in self.items.values() if i['speaker'] == 'sam' and not i['text'] and i.get('situations')]
        return sorted(cuts, key=lambda i: i['id']) + sorted(sam, key=lambda i: i['id'])

    def start_prefetch(self):
        """Decode every source into the cache, then (once per game version) measure them for the suggestions; both in
        the source's worker pool, pausable."""
        if self.source is None or not self.items or not hasattr(self.source, 'prefetch_pool'):
            return
        if self._prefetch_thread and self._prefetch_thread.is_alive():
            self._prefetch_paused.clear()
            self.prefetch['state'] = 'running'
            return
        todo = self.prefetch_order()
        cached = len(self.items) - len(todo)
        need_match = self.match is None and hasattr(self.source, 'analyse_submit')
        self.prefetch = {'state': 'running' if (todo or need_match) else 'done', 'done': cached,
                         'total': len(self.items), 'failed': 0, 'workers': self.workers}
        if not todo and not need_match:
            return
        self._prefetch_paused.clear()

        def pump(pool, queue, submit, on_result, counter):
            """Keep up to 2 jobs per worker running from `queue`; False when stopped."""
            from concurrent.futures import FIRST_COMPLETED, wait
            pending = {}
            while queue or pending:
                while queue and not self._prefetch_paused.is_set() and len(pending) < 2 * self.workers:
                    it = queue.popleft()
                    fut = submit(it)
                    if fut is None:
                        counter['done'] += 1
                        continue
                    pending[fut] = it
                if not pending:
                    time.sleep(0.3)   # paused with nothing running
                    if self.prefetch['state'] == 'stopped':
                        return False
                    continue
                finished, _ = wait(list(pending), timeout=0.5, return_when=FIRST_COMPLETED)
                for f in finished:
                    it = pending.pop(f)
                    try:
                        on_result(it, f.result())
                        counter['done'] += 1
                    except Exception as e:
                        counter['failed'] += 1
                        self.log('%s failed: %s' % (it['id'], e))
                if self.prefetch['state'] == 'stopped':
                    return False
            return True

        def run():
            from collections import deque
            try:
                pool = self._prefetch_pool = self.source.prefetch_pool(self.workers)
            except Exception as e:
                self.prefetch['state'] = 'error'
                self.log('background work could not start: %s\n%s' % (e, traceback.format_exc()))
                return
            try:
                def submit_decode(it):
                    path = self.ws.audio_path(it['id'])
                    return None if os.path.exists(path) else self.source.prefetch_submit(pool, it, path)
                if not pump(pool, deque(todo), submit_decode, lambda it, st: self._remember(it['id'], st),
                            self.prefetch):
                    return
                self._flush_measured()
                if need_match:
                    jobs = [i for i in self.analysis_jobs() if os.path.exists(self.ws.audio_path(i['id']))]
                    self.analysis = {'state': 'running', 'done': 0, 'total': len(jobs), 'failed': 0}
                    found = {'cands': [], 'sam': {}, 'her_f0': {}}

                    def submit_match(it):
                        kind = 'cutscene' if it['kind'] == 'cutscene' else 'line'
                        return self.source.analyse_submit(pool, kind, self.ws.audio_path(it['id']), it.get('subs'),
                                                          it['id'])

                    def got(it, res):
                        if it['kind'] == 'cutscene':
                            found['her_f0'][it['id']], cands = res
                            found['cands'] += cands
                        else:
                            found['sam'][it['id']] = res
                    if not pump(pool, deque(jobs), submit_match, got, self.analysis):
                        self.analysis['state'] = 'stopped'
                        return
                    self._set_match(dict(found, version=M.MATCH_VERSION, game=self.catalog.get('game')), save=True)
                    self.analysis['state'] = 'done'
                self.prefetch['state'] = 'done'
            finally:
                self._flush_measured()
                try:
                    pool.shutdown(wait=False, cancel_futures=True)
                except TypeError:   # Python before 3.9
                    pool.shutdown(wait=False)
                self._prefetch_pool = None
        self._prefetch_thread = threading.Thread(target=run, daemon=True)
        self._prefetch_thread.start()

    # ---- suggestions (vc_match): Fragile's sounds ranked for each of Sam's situations
    def _set_match(self, m, save=False):
        if save:
            ST.atomic_json(self.ws.match_path, m)
        rows = list(m.get('sam', {}).values()) + [c for c in m.get('cands', []) if c['kind'] != 'line']
        self._match_scales = M.scales(rows) if rows else None
        self._cands = {c['id']: c for c in m.get('cands', [])}
        self.match = m

    def load_match(self):
        try:
            with open(self.ws.match_path, encoding='utf-8') as f:
                m = json.load(f)
        except (OSError, ValueError):
            return
        if m.get('version') == M.MATCH_VERSION and self.catalog and m.get('game') == self.catalog.get('game'):
            self._set_match(m)

    def situation(self, h):
        for s in (self.catalog or {}).get('situations', []):
            if s['hash'] == h:
                return s
        return None

    def effective_category(self, s):
        return self.ws.state.get('situations', {}).get(str(s['hash'])) or s.get('guess', 'other')

    def sam_lines(self, h):
        return [i for i in self.items.values() if i['speaker'] == 'sam' and h in (i.get('situations') or [])]

    def suggestions(self, h, limit=6):
        """[(score, candidate, why)] for one of Sam's situations (empty before the analysis)."""
        s = self.situation(h)
        if not s or not self.match:
            return []
        lines = self.sam_lines(h)
        spoken = s.get('spoken', 0) * 2 > max(1, len(lines))
        prof = M.profile([self.match['sam'].get(i['id']) for i in lines if not i['text']])
        rejected = self.ws.state.get('rejected', {}).get(str(h), [])
        return M.suggest(prof, [i['text'] for i in lines if i['text']], spoken, self.effective_category(s),
                         list(self._cands.values()), self._match_scales, rejected, limit)

    def sam_loudness(self, hashes):
        vals = sorted(self.ws.measured[i['id']]['loud_db'] for h in hashes for i in self.sam_lines(h)
                      if i['id'] in self.ws.measured and self.ws.measured[i['id']]['loud_db'] > -119)
        return vals[len(vals) // 2] if vals else None

    def clip_from_candidate(self, cand_id, hashes, user=False):
        """A clip of one of her candidate sounds (a little room around it, short fades, the gain that brings it to
        Sam's loudness in those situations), made for `hashes`. An existing clip of the same sound is reused."""
        c = self._cands.get(cand_id)
        if not c:
            raise ST.ClipError('unknown suggestion')
        for old in self.ws.clips():
            if old.get('candidate') == cand_id:
                for h in hashes:
                    old = self.ws.link_clip(old['id'], h)
                if user and old.get('auto'):
                    old['auto'] = False
                    self.ws.save()
                return old
        cats = [self.effective_category(self.situation(h)) for h in hashes if self.situation(h)]
        cat = max(set(cats), key=cats.count) if cats else 'other'
        start = max(0.0, c['t0'] - 0.04)
        rate, seg, frames = self.source_range(c['item'], start, c['t1'] + 0.08)
        end = min(frames / float(rate), c['t1'] + 0.08)
        # the gain that makes it as loud as Sam's lines there (both measured by vc_audio.stats' loudness)
        target = self.sam_loudness(hashes)
        own = AU.stats(seg, rate)['loud_db'] if seg else -120.0
        gain = 0.0
        if target is not None and own > -119:
            gain = max(-24.0, min(24.0, round((target - own) * 2) / 2.0))
        body = {'source': c['item'], 'category': cat, 'start': start, 'end': end,
                'fade_in': 0.01, 'fade_out': 0.05, 'gain_db': gain, 'normalize': False,
                'note': c.get('text') or ('her %s' % c['kind']), 'situations': list(hashes), 'auto': not user,
                'candidate': cand_id}
        return self.save_clip(body)

    def autofill(self):
        """For every situation without an answer: link its best suggestion (one clip per sound, shared by all the
        situations it fits); they are marked for a listen in the guide."""
        if not self.match:
            raise ST.ClipError('the analysis has not finished yet')
        done = {h for c in self.ws.clips() for h in c.get('situations', [])}
        plans = self.ws.state.get('plan', {})
        picks = {}
        for s in self.catalog['situations']:
            h = s['hash']
            if s.get('lines', 0) <= 0 or h in done or str(h) in plans:
                continue
            sug = self.suggestions(h, limit=1)
            if sug and sug[0][0] >= M.AUTOFILL_MIN:
                picks.setdefault(sug[0][1]['id'], []).append(h)
        made = linked = 0
        for cand_id, hashes in picks.items():
            existed = any(c.get('candidate') == cand_id for c in self.ws.clips())
            self.clip_from_candidate(cand_id, hashes)
            made += 0 if existed else 1
            linked += len(hashes)
        return {'clips': made, 'situations': linked}

    def review(self, h, ok):
        """The guide's keep (ok=True) or 'another one' (ok=False: the suggestion is rejected for this situation, its
        clip unlinked, deleted if it was an unkept suggestion made for nothing else, and the next one linked)."""
        h = ST.clean_hash(h)
        rev = self.ws.state.setdefault('review', {})
        if ok:
            rev[str(h)] = 'ok'
            self.ws.save()
            return {'kept': h}
        nxt = None
        for c in list(self.ws.clips()):
            if h in c.get('situations', []) and c.get('auto'):
                rej = self.ws.state.setdefault('rejected', {}).setdefault(str(h), [])
                if c.get('candidate') and c['candidate'] not in rej:
                    rej.append(c['candidate'])
                c = self.ws.link_clip(c['id'], h, on=False)
                if not c.get('situations'):
                    self.ws.delete_clip(c['id'])
        rev.pop(str(h), None)
        self.ws.save()
        sug = self.suggestions(h, limit=1)
        if sug:
            nxt = self.clip_from_candidate(sug[0][1]['id'], [h])
        return {'next': nxt['id'] if nxt else None}

    def suggestion_view(self, h):
        out = []
        for score, c, why in self.suggestions(h):
            out.append({'id': c['id'], 'item': c['item'], 't0': c['t0'], 't1': c['t1'], 'kind': c['kind'],
                        'text': c.get('text', ''), 'dur': c['dur'], 'score': score, 'why': why,
                        'conf': c.get('conf'), 'loud_db': c.get('loud_db')})
        return out

    def pause_prefetch(self):
        if self.prefetch['state'] == 'running':
            self._prefetch_paused.set()
            self.prefetch['state'] = 'paused'

    def stop_prefetch(self):
        if self.prefetch['state'] in ('running', 'paused'):
            self._prefetch_paused.set()
            self.prefetch['state'] = 'stopped'

    def log(self, text):
        try:
            with open(os.path.join(self.ws.root, 'voicecut.log'), 'a', encoding='utf-8') as f:
                f.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), text))
        except OSError:
            pass

    # ---- source audio (decoded once, then cached as a WAV)
    def ensure_source(self, item_id):
        """The cached WAV of a source, decoding it first if needed (in this process: what you open now does not wait
        for the background queue)."""
        item = self.items.get(item_id)
        if item is None:
            raise KeyError(item_id)
        path = self.ws.audio_path(item_id)
        with self._lock:
            lock = self._decode_locks.setdefault(item_id, threading.Lock())
        with lock:
            if os.path.exists(path):
                try:
                    AU.wav_frames(path)
                    os.utime(path, None)
                    return path
                except (OSError, EOFError, ValueError, Exception):
                    pass   # unreadable (a killed write): decode again
            self.decoding[item_id] = 0.0
            try:
                rate, pcm = self.source.decode(item, lambda f: self.decoding.__setitem__(item_id, round(f, 3)))
            finally:
                self.decoding.pop(item_id, None)
            tmp = '%s.%d.tmp' % (path, threading.get_ident())
            AU.write_wav(tmp, pcm, rate)
            os.replace(tmp, path)
            self.ws.prune_cache(keep=[path])
            self._remember(item_id, AU.stats(pcm, rate))
            return path

    def source_range(self, item_id, start, end):
        """(rate, samples of [start, end) seconds, total frames) of a source."""
        return AU.read_wav_range(self.ensure_source(item_id), start, end)

    def source_wav(self, item_id):
        with open(self.ensure_source(item_id), 'rb') as f:
            return f.read()

    def _remember(self, item_id, st):
        """Keep a source's loudness; measure.json is written at most every 5 s (thousands come in while decoding)."""
        with self._lock:
            self.ws.measured[item_id] = st
            self._measure_dirty += 1
            if time.time() - self._measure_saved < 5:
                return
        self._flush_measured()

    def _flush_measured(self):
        with self._lock:
            if self._measure_dirty:
                self.ws.save_measured()
                self._measure_dirty = 0
            self._measure_saved = time.time()

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
                            break
                        i = self._measure_queue[0]
                    try:
                        self.ensure_source(i)
                    except Exception:
                        self.log('measure %s failed\n%s' % (i, traceback.format_exc()))
                    with self._lock:   # off the queue only when done, so the count includes the line in hand
                        if self._measure_queue and self._measure_queue[0] == i:
                            self._measure_queue.pop(0)
                self._flush_measured()
            self._measure_thread = threading.Thread(target=run, daemon=True)
            self._measure_thread.start()
        return len(self._measure_queue)

    # ---- API
    def status(self):
        clips = self.ws.clips()
        return {'app': APP, 'version': VERSION, 'catalog': self.cat_state, 'game_dir': self.game_dir,
                'workspace': self.ws.root, 'export_dir': self.ws.export_dir, 'language': self.language,
                'clips': len(clips), 'decoding': dict(self.decoding), 'measuring': len(self._measure_queue),
                'prefetch': dict(self.prefetch), 'analysis': dict(self.analysis)}

    def catalog_view(self):
        if not self.catalog:
            return {'ready': False}
        keep = ('id', 'kind', 'speaker', 'text', 'length', 'group', 'situations', 'category', 'sure', 'name', 'lines',
                'subs')
        return {'ready': True, 'items': [{k: i[k] for k in keep if k in i} for i in self.catalog['items']],
                'situations': self.catalog['situations'], 'categories': self.catalog['categories'],
                'built': self.catalog.get('built'), 'seconds': self.catalog.get('seconds')}

    def user_view(self):
        with self._lock:
            measured = dict(self.ws.measured)
        return {'clips': self.ws.clips(), 'situations': self.ws.state.get('situations', {}),
                'plan': self.ws.state.get('plan', {}), 'review': self.ws.state.get('review', {}),
                'rejected': self.ws.state.get('rejected', {}), 'measured': measured, 'analysed': bool(self.match)}

    def save_clip(self, body):
        c = ST.clean_clip(body, set(self.items) if self.items else None)
        rate, seg, frames = self.source_range(c['source'], c['start'], c['end'])
        if c['end'] > frames / rate + 0.001:
            raise ST.ClipError('the selection ends after the line')
        pcm = AU.render(seg, rate, dict(c, start=0.0, end=len(seg) / float(rate)))
        if not pcm:
            raise ST.ClipError('the selection is outside the line')
        return self.ws.upsert_clip(c, set(self.items) if self.items else None, AU.stats(pcm, rate))

    def situation_rows(self):
        """(hash, category, user|known|guess, label, plan) of Sam's situations: the user's sorting, else the guess;
        plan is the guide's answer: clip (clips were made for it), any (any clip of the category), silent, or -."""
        rows = []
        user = self.ws.state.get('situations', {})
        plans = self.ws.state.get('plan', {})
        targeted = {h for c in self.ws.clips() for h in c.get('situations', [])}
        for s in (self.catalog or {}).get('situations', []):
            u = user.get(str(s['hash']))
            how = 'user' if u else ('known' if s.get('known') else 'guess')
            plan = 'clip' if s['hash'] in targeted else plans.get(str(s['hash']), '-')
            rows.append((s['hash'], u or s.get('guess', 'other'), how, s.get('label', ''), plan))
        return rows

    def export(self):
        stamp = self.source.stamp() if self.source else ''
        return self.ws.export(self.source_range, stamp, self.language, situations=self.situation_rows())

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
            if path == '/api/suggest':
                try:
                    h = ST.clean_hash((query.get('hash') or [''])[0])
                except ST.ClipError as e:
                    return 400, {'error': str(e)}
                return 200, {'ready': bool(self.match), 'suggestions': self.suggestion_view(h)}
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
                if path == '/api/plan':
                    self.ws.set_plan(body.get('hash', -1), body.get('plan'))
                    return 200, {'plan': self.ws.state.get('plan', {})}
                if path == '/api/clip/link':
                    c = self.ws.link_clip(str(body.get('id', '')), body.get('hash', -1), bool(body.get('on', True)))
                    return 200, c
                if path == '/api/autofill':
                    return 200, self.autofill()
                if path == '/api/review':
                    return 200, self.review(body.get('hash', -1), bool(body.get('ok')))
                if path == '/api/use':
                    h = ST.clean_hash(body.get('hash', -1))
                    c = self.clip_from_candidate(str(body.get('candidate', '')), [h], user=True)
                    self.review(h, True)
                    return 200, c
                if path == '/api/prefetch':
                    action = body.get('action')
                    if action == 'pause':
                        self.pause_prefetch()
                    elif action == 'resume':
                        self.start_prefetch()
                    else:
                        return 400, {'error': 'action is pause or resume'}
                    return 200, {'prefetch': dict(self.prefetch)}
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
    app = App(ws, source, args.language, game_dir, prefetch=not args.no_prefetch, workers=args.workers)
    Handler.app = app
    Handler.port = port
    app.start_catalog()
    url = 'http://127.0.0.1:%d/' % port
    print('voicecut: %s (Ctrl+C to stop; exits after %d s without a request)' % (url, args.idle_exit))
    sys.stdout.flush()

    def idle_watch():
        while True:
            time.sleep(10)
            # the background decode finishes first (it is what the next start would otherwise redo)
            busy = app.decoding or app.cat_state['state'] == 'building' or app.prefetch['state'] == 'running'
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
        app.stop_prefetch()
        app._flush_measured()
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
    ap.add_argument('--no-prefetch', action='store_true', help='do not decode every line in the background')
    ap.add_argument('--workers', type=int, help='background decoding processes (default: CPU threads - 2, 1..6)')
    return serve(ap.parse_args(argv))


if __name__ == '__main__':
    sys.exit(main())
