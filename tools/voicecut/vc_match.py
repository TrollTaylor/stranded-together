"""voicecut: find Fragile's wordless sounds in her cutscenes and rank them against Sam's situations, by measurement.
Python 3 standard library only. Nothing here listens: it measures, and the guide asks you to keep or reject.

Finding her sounds (a cutscene's dialogue track holds every voice):
  - voice activity: 10 ms frames, active above (the 98th percentile energy - 40 dB), joined over gaps under 120 ms;
  - the subtitle timeline says who speaks a line; a sound outside every subtitle (+-50 ms) is a candidate;
  - pitch (autocorrelation, 70-420 Hz, on an 8 kHz copy) tells her voice from the men's: her subtitled speech in
    the cutscene gives her median F0 (about 180 Hz), Sam's about 90 Hz; a voiced sound nearer hers is hers ('effort');
  - an unvoiced sound (breath) is hers only by place: within 3 s of one of her lines and nearer to hers than to anyone
    else's ('breath');
  - her short subtitled lines are candidates too ('line'), matched by words for Sam's spoken situations.

Matching: each sound gets a few numbers (duration, voiced share, pitch against the speaker's own median, pitch slope,
attack, end decay, noisiness); a Sam situation's profile is the mean over his lines; suggestions are the nearest
candidates of the kinds that fit (wordless situations: efforts and breaths; spoken: lines by word overlap).
"""
import array
import math
import operator
import re
import statistics
import sys
import wave

SR = 8000
MATCH_VERSION = 1
FEATURES = ('ldur', 'voiced', 'pitch', 'slope', 'attack', 'decay', 'noise')
WEIGHTS = {'ldur': 1.4, 'voiced': 2.0, 'pitch': 0.8, 'slope': 0.4, 'attack': 0.8, 'decay': 0.6, 'noise': 0.9}
VOICED = 0.25   # a sound with at least this share of pitched frames is voiced
AUTOFILL_MIN = 0.3   # the auto-fill links a suggestion only from this score (0..1)


def load_8k(path, start=0.0, end=None):
    """A 16-bit mono WAV (the cache) as 8 kHz floats (6-sample means of the 48 kHz source: low-pass enough for pitch
    and energy). Returns (rate, samples)."""
    with wave.open(path, 'rb') as w:
        rate = w.getframerate()
        n = w.getnframes()
        a = max(0, int(start * rate))
        b = n if end is None else min(n, int(end * rate))
        w.setpos(a)
        raw = w.readframes(max(0, b - a))
    x = array.array('h')
    x.frombytes(raw[:len(raw) // 2 * 2])
    if sys.byteorder != 'little':
        x.byteswap()
    step = max(1, rate // SR)
    k = 1.0 / (step * 32767.0)
    m = len(x) // step
    return rate // step, array.array('f', (sum(x[i * step:(i + 1) * step]) * k for i in range(m)))


def frame_energy(x, sr=SR, hop=0.01, win=0.03):
    h, n = int(sr * hop), int(sr * win)
    return [sum(map(operator.mul, x[i:i + n], x[i:i + n])) / n for i in range(0, max(0, len(x) - n), h)]


def active_segments(en, hop=0.01, rel_db=-40.0, minlen=0.1, maxgap=0.12, floor=None):
    """[(t0, t1)] where the energy is within rel_db of the track's 98th percentile (and above `floor`)."""
    if not en:
        return []
    top = sorted(en)[int(len(en) * 0.98)]
    thr = max(top * 10 ** (rel_db / 10.0), floor or 0.0)
    segs, s, last = [], None, None
    for i, e in enumerate(en):
        if e > thr:
            if s is None:
                s = i
            last = i
        elif s is not None and (i - last) * hop > maxgap:
            segs.append((round(s * hop, 3), round((last + 1) * hop, 3)))
            s = None
    if s is not None:
        segs.append((round(s * hop, 3), round((last + 1) * hop, 3)))
    return [g for g in segs if g[1] - g[0] >= minlen]


def pitch_track(x, sr=SR, t0=0.0, t1=None, fmin=70, fmax=420, hop=0.02, win=0.04, clarity=0.55):
    """F0 per frame (None when unvoiced): the lag with the highest normalised autocorrelation."""
    n, lo, hi = int(sr * win), int(sr / fmax), int(sr / fmin)
    end = len(x) if t1 is None else min(len(x), int(t1 * sr))
    out = []
    i = int(t0 * sr)
    while i + n + hi <= end:
        seg = x[i:i + n]
        e0 = sum(map(operator.mul, seg, seg))
        f = None
        if e0 > 1e-7:
            best, bl = 0.0, 0
            for lag in range(lo, hi):
                s2 = x[i + lag:i + lag + n]
                c = sum(map(operator.mul, seg, s2)) / math.sqrt(e0 * sum(map(operator.mul, s2, s2)) + 1e-12)
                if c > best:
                    best, bl = c, lag
            if best >= clarity and bl:
                f = sr / float(bl)
        out.append(f)
        i += int(sr * hop)
    return out


def median_f0(x, spans, sr=SR, limit=10):
    """The median F0 over the voiced frames of (up to `limit`) spans, or None."""
    vals = []
    for a, b in spans[:limit]:
        vals += [f for f in pitch_track(x, sr, a, b) if f]
    return statistics.median(vals) if vals else None


def features(x, sr, a, b, ref_f0):
    """The numbers a sound is matched by (see the module docstring)."""
    seg = x[int(a * sr):int(b * sr)]
    n = len(seg)
    dur = n / float(sr)
    track = pitch_track(x, sr, a, b)
    voiced = [f for f in track if f]
    vshare = len(voiced) / float(max(1, len(track)))
    is_voiced = vshare >= VOICED
    pitch = math.log2(statistics.median(voiced) / ref_f0) if is_voiced and voiced and ref_f0 else 0.0
    slope = 0.0
    if is_voiced and len(voiced) >= 3:   # pitch and its slope mean nothing on a breath
        k = len(voiced) // 3 or 1
        slope = math.log2((sum(voiced[-k:]) / k) / (sum(voiced[:k]) / k))
    en = frame_energy(seg, sr) or [0.0]
    peak_i = max(range(len(en)), key=lambda i: en[i])
    attack = peak_i / float(max(1, len(en) - 1))
    third = max(1, len(en) // 3)
    head, tail = sum(en[:third]) / third, sum(en[-third:]) / third
    decay = math.log10((tail + 1e-9) / (head + 1e-9))
    zc = sum(1 for i in range(1, n) if (seg[i] < 0) != (seg[i - 1] < 0)) / float(max(1, n))
    loud = math.sqrt(max(en))
    return {'dur': round(dur, 3), 'ldur': round(math.log2(max(dur, 0.05)), 3), 'voiced': round(vshare, 3), 'pitch': round(pitch, 3), 'slope': round(slope, 3),
            'attack': round(attack, 3), 'decay': round(max(-3.0, min(3.0, decay)), 3), 'noise': round(zc, 4),
            'loud_db': round(20 * math.log10(loud), 2) if loud > 1e-6 else -120.0}


def trim_active(x, sr=SR):
    """(t0, t1) of the active part of a whole line (its silence trimmed)."""
    segs = active_segments(frame_energy(x, sr), rel_db=-35.0, minlen=0.02, maxgap=0.3)
    if not segs:
        return 0.0, len(x) / float(sr)
    return segs[0][0], segs[-1][1]


SAM_F0 = 95.0   # used until a cutscene measures him


def analyse_line(path, ref_f0=SAM_F0):
    """Features of a whole line (one of Sam's), its silence trimmed."""
    sr, x = load_8k(path)
    a, b = trim_active(x, sr)
    f = features(x, sr, a, b, ref_f0)
    f['t0'], f['t1'] = a, b
    return f


def _who_near(subs, a, b, reach=3.0):
    """The speaker of the nearest subtitle within `reach` seconds of [a, b], and its distance."""
    best = (None, reach + 1)
    for s in subs:
        d = s['t0'] - b if s['t0'] >= b else (a - s['t1'] if s['t1'] <= a else 0.0)
        if 0 <= d < best[1]:
            best = (s['who'], d)
    return best if best[1] <= reach else (None, None)


def analyse_cutscene(path, subs, item_id, max_len=2.5):
    """Fragile's candidate sounds in one cutscene's dialogue track: (her median F0, [candidates])."""
    sr, x = load_8k(path)
    spans = lambda who: [(s['t0'], s['t1']) for s in subs if s['who'] == who]  # noqa: E731
    frg_f0 = median_f0(x, spans('fragile')) or 180.0
    men = median_f0(x, spans('sam')) or SAM_F0
    split = math.sqrt(frg_f0 * men)   # geometric middle between her pitch and Sam's
    others = median_f0(x, spans('other'))
    female_others = others is not None and others > split   # another woman speaks in this cutscene
    out = []
    for a, b in active_segments(frame_energy(x, sr), rel_db=-42.0):
        if b - a > max_len:
            continue
        over = [s for s in subs if s['t0'] - 0.05 < b and s['t1'] + 0.05 > a]
        if any(s['who'] == 'fragile' for s in over):
            continue              # her words: the line candidates below
        if female_others and any(s['who'] == 'other' for s in over):
            continue              # over another woman's line: likely hers
        f = features(x, sr, a, b, frg_f0)
        who, dist = _who_near(subs, a, b)
        if f['voiced'] >= VOICED:
            f0 = frg_f0 * 2 ** f['pitch']
            if f0 < split * (1.15 if over else 1.0):
                continue          # a man's voice (over someone else's line: clearly hers only)
            kind = 'effort'
            conf = min(1.0, 0.6 + 0.4 * min(1.0, (f0 - split) / max(1.0, frg_f0 - split)))
            if over:
                conf -= 0.25      # a sound of hers while someone else talks
            elif who not in (None, 'fragile'):
                conf -= 0.15
        else:
            if over or who != 'fragile':
                continue          # a breath nearer someone else's line
            kind = 'breath'
            conf = 0.5 + 0.3 * (1 - dist / 3.0)
        out.append(dict(f, id='%s@%.2f' % (item_id, a), item=item_id, t0=a, t1=b, kind=kind, conf=round(conf, 2)))
    for s in subs:   # her short lines: reactions and spoken matches
        if s['who'] == 'fragile' and s['t1'] - s['t0'] <= 2.0 and len(s['text'].split()) <= 4:
            f = features(x, sr, s['t0'], s['t1'], frg_f0)
            out.append(dict(f, id='%s@%.2f' % (item_id, s['t0']), item=item_id, t0=s['t0'], t1=s['t1'], kind='line',
                            text=s['text'].strip(), conf=0.9))
    return round(frg_f0, 1), out


# ---------------------------------------------------------------- matching

WORD = re.compile(r"[a-z']+")


def words(text):
    return {w.strip("'") for w in WORD.findall((text or '').lower())} - {''}


def text_score(a, b):
    """Word overlap (Jaccard) of two lines."""
    wa, wb = words(a), words(b)
    if not wa or not wb:
        return 0.0
    return len(wa & wb) / float(len(wa | wb))


def scales(feature_rows):
    """Per-feature spread (standard deviation, at least a floor) over every row, to compare features fairly."""
    out = {}
    for k in FEATURES:
        vals = [r[k] for r in feature_rows if k in r]
        out[k] = max(statistics.pstdev(vals) if len(vals) > 1 else 1.0, 0.05)
    return out


def profile(rows):
    """The mean features of Sam's lines in a situation."""
    rows = [r for r in rows if r]
    if not rows:
        return None
    return {k: sum(r[k] for r in rows) / len(rows) for k in FEATURES + ('loud_db', 'dur')}


def distance(p, c, sc):
    return math.sqrt(sum(WEIGHTS[k] * ((p[k] - c[k]) / sc[k]) ** 2 for k in FEATURES))


KINDS_FOR = {   # which kinds of her sounds fit a category (wordless situations)
    'breathing': ('breath', 'effort'), 'rest': ('breath', 'effort'), 'cold_water': ('breath', 'effort'),
    'hum': ('effort',),
}


def suggest(prof, sam_texts, spoken, category, candidates, sc, rejected=(), limit=6):
    """[(score 0..1, candidate, why)] best first. Wordless situations: efforts and breaths by distance (breaths only for
    the breathing-like categories unless nothing else is near); spoken ones: her lines by word overlap."""
    out = []
    rej = set(rejected)
    if spoken:
        for c in candidates:
            if c['kind'] != 'line' or c['id'] in rej:
                continue
            t = max((text_score(c.get('text', ''), s) for s in sam_texts), default=0.0)
            if t > 0:
                out.append((round(t, 3), c, 'same words as Sam\'s line'))
        out.sort(key=lambda r: -r[0])
        return out[:limit]
    if not prof:
        return []
    kinds = KINDS_FOR.get(category, ('effort', 'breath'))
    for c in candidates:
        if c['id'] in rej or c['kind'] not in kinds:
            continue
        d = distance(prof, c, sc)
        score = math.exp(-d / 3.0) * c.get('conf', 0.5)
        if c['kind'] == 'breath' and prof['voiced'] >= 0.4:
            score *= 0.4           # Sam makes a voiced sound here
        elif c['kind'] == 'effort' and prof['voiced'] < 0.15:
            score *= 0.5           # Sam only breathes here
        why = '%s, %.2f s against Sam\'s %.2f s, %s' % (c['kind'], c['dur'], prof['dur'],
                                                       'voiced' if c['voiced'] >= 0.25 else 'breathy')
        out.append((round(score, 3), c, why))
    out.sort(key=lambda r: -r[0])
    return out[:limit]
