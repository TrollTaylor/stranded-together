"""voicecut: the user's work (clips, Sam's situation categories) and the export the mod reads. Python 3 stdlib only.

Workspace (per user, never in the repo): %LOCALAPPDATA%\\Stranded Together\\VoiceCut
  state.json        clips and situation categories (written atomically; a .bak of the previous version is kept)
  catalog.json      the scanned catalog (rebuilt when ds.exe changes)
  audio\\<id>.wav   decoded sources (16-bit mono 48 kHz), a cache: pruned to CACHE_LIMIT bytes, oldest first
  measure.json      loudness of measured sources

Export (per user): %LOCALAPPDATA%\\Stranded Together\\Voices
  voices.txt        the manifest (UTF-8, tab-separated; see MANIFEST_HEADER and docs/voicecut.md)
  fragile\\<category>\\<category>_NN.wav   16-bit mono 48 kHz PCM
An export replaces only the files the previous voices.txt listed; anything else in the folder is left alone.
"""
import hashlib
import json
import os
import re
import time

import vc_audio as AU
from vc_catalog import CATEGORY_KEYS

STATE_VERSION = 1
MANIFEST_VERSION = 1
CACHE_LIMIT = 3 * 1024 * 1024 * 1024   # every line decoded is about 0.8 GB
CLIP_ID = re.compile(r'^clip-[0-9a-f]{8}$')
ITEM_ID = re.compile(r'^[a-z]-[0-9A-Za-z_\-]{1,80}$')
PLANS = ('any', 'silent')   # the guide's answers besides "clips made for it": any clip of the category / no voice
MANIFEST_HEADER = ('# clip\t<file>\t<category>\t<samples>\t<peak dBFS>\t<loud dBFS>\t<source id>\t<start s>\t<end s>'
                   '\t<fade in s>\t<fade out s>\t<gain dB>\t<normalise 0|1>\t<sha256 of the file>'
                   '\t<made for: Sam situation hashes, comma-separated, or ->\n'
                   '# situation\t<Sam situation hash, 8 hex>\t<category>\t<user|known|guess>\t<label>'
                   '\t<plan: clip|any|silent|->')


def clean_hash(h):
    try:
        h = int(h)
    except (TypeError, ValueError):
        raise ClipError('bad situation hash')
    if not 0 <= h < 0x80000000:
        raise ClipError('bad situation hash')
    return h


def default_workspace():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.environ.get('ST_VOICECUT_DIR') or os.path.join(base, 'Stranded Together', 'VoiceCut')


def default_export():
    base = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
    return os.environ.get('ST_VOICES_DIR') or os.path.join(base, 'Stranded Together', 'Voices')


def atomic_json(path, obj, backup=False):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)
    if backup and os.path.exists(path):
        try:
            os.replace(path, path + '.bak')
        except OSError:
            pass
    os.replace(tmp, path)


def safe_name(item_id):
    return re.sub(r'[^0-9A-Za-z_\-]', '_', item_id)


class ClipError(ValueError):
    pass


def _num(clip, key, lo, hi, default=0.0):
    v = clip.get(key, default)
    try:
        v = float(v)
    except (TypeError, ValueError):
        raise ClipError('%s must be a number' % key)
    if v != v or not lo <= v <= hi:
        raise ClipError('%s must be between %g and %g' % (key, lo, hi))
    return round(v, 4)


def clean_clip(clip, known_sources=None):
    """A clip as stored: checked fields only."""
    if not isinstance(clip, dict):
        raise ClipError('a clip is an object')
    src = str(clip.get('source', ''))
    if not ITEM_ID.match(src) or (known_sources is not None and src not in known_sources):
        raise ClipError('unknown source line')
    cat = str(clip.get('category', ''))
    if cat not in CATEGORY_KEYS:
        raise ClipError('unknown category')
    out = {'source': src, 'category': cat,
           'start': _num(clip, 'start', 0, 36000), 'end': _num(clip, 'end', 0, 36000),
           'fade_in': _num(clip, 'fade_in', 0, 10), 'fade_out': _num(clip, 'fade_out', 0, 10),
           'gain_db': _num(clip, 'gain_db', -40, 40), 'normalize': bool(clip.get('normalize')),
           'note': str(clip.get('note', ''))[:300]}
    if out['end'] - out['start'] < 0.02:
        raise ClipError('the selection is shorter than 20 ms')
    if out['end'] - out['start'] > 30:
        raise ClipError('a clip is at most 30 s long')
    sits = clip.get('situations') or []
    if not isinstance(sits, list) or len(sits) > 64:
        raise ClipError('situations is a list of at most 64 hashes')
    out['situations'] = sorted({clean_hash(h) for h in sits})
    cid = clip.get('id')
    if cid:
        if not CLIP_ID.match(str(cid)):
            raise ClipError('bad clip id')
        out['id'] = str(cid)
    return out


class Workspace:
    def __init__(self, root=None, export_dir=None):
        self.root = root or default_workspace()
        self.export_dir = export_dir or default_export()
        self.audio_dir = os.path.join(self.root, 'audio')
        self.state_path = os.path.join(self.root, 'state.json')
        self.catalog_path = os.path.join(self.root, 'catalog.json')
        self.measure_path = os.path.join(self.root, 'measure.json')
        os.makedirs(self.audio_dir, exist_ok=True)
        self.state = self._load(self.state_path, {'version': STATE_VERSION, 'clips': [], 'situations': {}})
        self.measured = self._load(self.measure_path, {})

    @staticmethod
    def _load(path, default):
        for p in (path, path + '.bak'):
            try:
                with open(p, encoding='utf-8') as f:
                    v = json.load(f)
                if isinstance(v, dict):
                    return v
            except (OSError, ValueError):
                continue
        return default

    def save(self):
        atomic_json(self.state_path, self.state, backup=True)

    def save_measured(self):
        atomic_json(self.measure_path, self.measured)

    # ---- clips
    def clips(self):
        return self.state.setdefault('clips', [])

    def upsert_clip(self, clip, known_sources=None, stats=None):
        c = clean_clip(clip, known_sources)
        now = int(time.time())
        clips = self.clips()
        if 'id' in c:
            for i, old in enumerate(clips):
                if old.get('id') == c['id']:
                    c['created'] = old.get('created', now)
                    c['updated'] = now
                    if stats:
                        c['stats'] = stats
                    clips[i] = c
                    self.save()
                    return c
            raise ClipError('no such clip')
        used = {x.get('id') for x in clips}
        while True:
            cid = 'clip-' + hashlib.sha1(('%s%s%d' % (c['source'], time.time(), len(used))).encode()).hexdigest()[:8]
            if cid not in used:
                break
        c['id'] = cid
        c['created'] = c['updated'] = now
        if stats:
            c['stats'] = stats
        clips.append(c)
        self.save()
        return c

    def delete_clip(self, cid):
        clips = self.clips()
        n = len(clips)
        self.state['clips'] = [c for c in clips if c.get('id') != cid]
        if len(self.state['clips']) == n:
            raise ClipError('no such clip')
        self.save()

    # ---- Sam's situation categories (the user's own sorting; None removes it)
    def set_plan(self, h, plan):
        """The guide's answer for a Sam situation: 'any' (any clip of its category will do), 'silent' (no voice
        there), None (not decided). Clips made for it (link_clip) count on their own."""
        h = clean_hash(h)
        plans = self.state.setdefault('plan', {})
        if plan in (None, ''):
            plans.pop(str(h), None)
        elif plan in PLANS:
            plans[str(h)] = plan
        else:
            raise ClipError('plan is any, silent or empty')
        self.save()

    def link_clip(self, cid, h, on=True):
        """Mark a clip as made for a Sam situation (or not)."""
        h = clean_hash(h)
        for c in self.clips():
            if c.get('id') == cid:
                sits = set(c.get('situations', []))
                if on:
                    if len(sits) >= 64 and h not in sits:
                        raise ClipError('a clip is made for at most 64 situations')
                    sits.add(h)
                else:
                    sits.discard(h)
                c['situations'] = sorted(sits)
                self.save()
                return c
        raise ClipError('no such clip')

    def set_situation(self, h, category):
        h = clean_hash(h)
        sits = self.state.setdefault('situations', {})
        if category in (None, ''):
            sits.pop(str(h), None)
        elif category in CATEGORY_KEYS:
            sits[str(h)] = category
        else:
            raise ClipError('unknown category')
        self.save()

    # ---- decoded sources cache
    def audio_path(self, item_id):
        return os.path.join(self.audio_dir, safe_name(item_id) + '.wav')

    def prune_cache(self, keep=()):
        try:
            files = [os.path.join(self.audio_dir, n) for n in os.listdir(self.audio_dir) if n.endswith('.wav')]
        except OSError:
            return
        info = []
        for p in files:
            try:
                st = os.stat(p)
                info.append((st.st_atime, st.st_size, p))
            except OSError:
                pass
        total = sum(s for _, s, _ in info)
        keep = {os.path.normcase(p) for p in keep}
        for _, size, p in sorted(info):
            if total <= CACHE_LIMIT:
                break
            if os.path.normcase(p) in keep:
                continue
            try:
                os.remove(p)
                total -= size
            except OSError:
                pass

    # ---- export
    def export(self, load_source, game='', language='english', out_dir=None, situations=()):
        """Render every clip into out_dir (default: the export folder) and write voices.txt. `load_source(id, start,
        end)` returns (rate, mono floats of [start, end) seconds, the source's total frames). `situations`: (hash, category, how, label, plan) rows for Sam's situations, so the mod
        can map the situation Sam would speak in to a clip category (or to the clips made for it, or to silence).
        Returns a summary."""
        out = out_dir or self.export_dir
        os.makedirs(out, exist_ok=True)
        manifest = os.path.join(out, 'voices.txt')
        old = read_manifest(manifest)
        for row in old:
            p = os.path.normpath(os.path.join(out, row['file']))
            if os.path.commonpath([os.path.abspath(p), os.path.abspath(out)]) == os.path.abspath(out):
                try:
                    os.remove(p)
                except OSError:
                    pass
        rows = []
        counts = {}
        order = sorted(self.clips(), key=lambda c: (CATEGORY_KEYS.index(c['category']), c.get('created', 0), c['id']))
        errors = []
        for c in order:
            try:
                rate, seg, _frames = load_source(c['source'], c['start'], c['end'])
            except Exception as e:  # a source that no longer decodes: skip that clip, say so
                errors.append('%s: %s' % (c['id'], e))
                continue
            pcm = AU.render(seg, rate, dict(c, start=0.0, end=len(seg) / float(rate)))
            if not pcm:
                errors.append('%s: empty after trimming' % c['id'])
                continue
            counts[c['category']] = counts.get(c['category'], 0) + 1
            rel = os.path.join('fragile', c['category'], '%s_%02d.wav' % (c['category'], counts[c['category']]))
            path = os.path.join(out, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            data = AU.write_wav(path, pcm, rate)
            st = AU.stats(pcm, rate)
            rows.append([rel, c['category'], str(len(pcm)), '%.2f' % st['peak_db'], '%.2f' % st['loud_db'],
                         c['source'], '%.4f' % c['start'], '%.4f' % c['end'], '%.4f' % c['fade_in'],
                         '%.4f' % c['fade_out'], '%.2f' % c['gain_db'], '1' if c['normalize'] else '0',
                         hashlib.sha256(data).hexdigest(),
                         ','.join('%08X' % h for h in c.get('situations', [])) or '-'])
        lines = ['stv-voices %d' % MANIFEST_VERSION,
                 'game\t%s\ttool\tvoicecut %d\tlanguage\t%s\trate\t%d\tspeaker\tfragile' % (game or '-', STATE_VERSION,
                                                                                         language, AU.RATE),
                 MANIFEST_HEADER]
        lines += ['clip\t' + '\t'.join(r) for r in rows]
        sit_rows = 0
        for row in situations:
            h, cat, how, label = row[:4]
            plan = row[4] if len(row) > 4 else '-'
            if cat in CATEGORY_KEYS and (cat != 'other' or plan != '-'):
                lines.append('situation\t%08X\t%s\t%s\t%s\t%s' % (int(h), cat, how, re.sub(r'[\t\r\n]', ' ', label),
                                                                 plan))
                sit_rows += 1
        tmp = manifest + '.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        os.replace(tmp, manifest)
        return {'dir': out, 'manifest': manifest, 'clips': len(rows), 'by_category': counts, 'errors': errors,
                'situations': sit_rows}


def read_manifest(path):
    """The clip rows of a voices.txt ([] when missing or another version)."""
    try:
        with open(path, encoding='utf-8') as f:
            text = f.read()
    except OSError:
        return []
    lines = text.split('\n')
    if not lines or lines[0].strip() != 'stv-voices %d' % MANIFEST_VERSION:
        return []
    rows = []
    for ln in lines[1:]:
        parts = ln.rstrip('\r').split('\t')
        if parts[0] != 'clip' or len(parts) < 14:
            continue
        f = parts[1].replace('/', os.sep).replace('\\', os.sep)
        if os.path.isabs(f) or '..' in f.split(os.sep):
            continue
        rows.append({'file': f, 'category': parts[2], 'samples': int(parts[3]), 'source': parts[6],
                     'sha256': parts[13]})
    return rows
