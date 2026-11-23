"""voicecut: mono PCM helpers: WAV in and out, the clip render (trim, normalise, gain, fades) and loudness figures.
Python 3 standard library only. app.js renders its preview with the same math (renderClip), so what you hear while
editing is what the export writes.
"""
import array
import io
import math
import struct
import sys
import wave

RATE = 48000
NORMALIZE_PEAK_DB = -3.0     # "normalise" brings the peak here before the gain
MIN_DB = -120.0


def db(x):
    return round(20 * math.log10(x), 2) if x > 1e-6 else MIN_DB


def to_int16(samples):
    a = array.array('h', (max(-32767, min(32767, int(round(v * 32767.0)))) for v in samples))
    if sys.byteorder != 'little':
        a.byteswap()
    return a.tobytes()


def wav_bytes(samples, rate=RATE):
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(to_int16(samples))
    return buf.getvalue()


def write_wav(path, samples, rate=RATE):
    data = wav_bytes(samples, rate)
    with open(path, 'wb') as f:
        f.write(data)
    return data


def read_wav(path_or_bytes):
    """(rate, mono floats) of a 16-bit PCM WAV (channels averaged)."""
    src = io.BytesIO(path_or_bytes) if isinstance(path_or_bytes, (bytes, bytearray)) else path_or_bytes
    with wave.open(src, 'rb') as w:
        if w.getsampwidth() != 2:
            raise ValueError('only 16-bit WAV files are read')
        ch = w.getnchannels()
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    a = array.array('h')
    a.frombytes(raw[:len(raw) // 2 * 2])
    if sys.byteorder != 'little':
        a.byteswap()
    if ch == 1:
        return rate, [v / 32767.0 for v in a]
    return rate, [sum(a[i:i + ch]) / (32767.0 * ch) for i in range(0, len(a) - ch + 1, ch)]


def wav_frames(path):
    """(rate, frames) of a WAV file."""
    with wave.open(path, 'rb') as w:
        return w.getframerate(), w.getnframes()


def read_wav_range(path, start, end):
    """(rate, mono floats of [start, end) seconds, total frames) of a 16-bit mono WAV, reading only that range (a
    cutscene's cache is minutes long; a clip needs a second of it)."""
    with wave.open(path, 'rb') as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            raise ValueError('the cache holds 16-bit mono WAV files')
        rate, frames = w.getframerate(), w.getnframes()
        a = max(0, min(frames, int(round(start * rate))))
        b = max(a, min(frames, int(round(end * rate))))
        w.setpos(a)
        raw = w.readframes(b - a)
    x = array.array('h')
    x.frombytes(raw[:len(raw) // 2 * 2])
    if sys.byteorder != 'little':
        x.byteswap()
    return rate, [v / 32767.0 for v in x], frames


def fade_gain(i, n, fade_in, fade_out):
    """The fade factor of sample i of n (raised-cosine ramps of fade_in / fade_out samples)."""
    g = 1.0
    if fade_in > 0 and i < fade_in:
        g *= 0.5 - 0.5 * math.cos(math.pi * (i + 0.5) / fade_in)
    if fade_out > 0 and i >= n - fade_out:
        g *= 0.5 - 0.5 * math.cos(math.pi * (n - i - 0.5) / fade_out)
    return g


def render(source, rate, clip):
    """A clip from its source: trim to [start, end) seconds, normalise the peak to NORMALIZE_PEAK_DB if asked, apply
    gain_db, then the fades (seconds); clamped to -1..1."""
    a = max(0, int(round(float(clip.get('start', 0)) * rate)))
    b = min(len(source), int(round(float(clip.get('end', 0)) * rate)))
    seg = source[a:b] if b > a else []
    n = len(seg)
    if not n:
        return []
    k = 10 ** (float(clip.get('gain_db', 0) or 0) / 20.0)
    if clip.get('normalize'):
        peak = max(abs(v) for v in seg)
        if peak > 1e-6:
            k *= 10 ** (NORMALIZE_PEAK_DB / 20.0) / peak
    fi = min(n, int(round(float(clip.get('fade_in', 0) or 0) * rate)))
    fo = min(n, int(round(float(clip.get('fade_out', 0) or 0) * rate)))
    out = []
    for i, v in enumerate(seg):
        g = k
        if (fi and i < fi) or (fo and i >= n - fo):
            g *= fade_gain(i, n, fi, fo)
        x = v * g
        out.append(1.0 if x > 1.0 else (-1.0 if x < -1.0 else x))
    return out


def stats(samples, rate=RATE):
    """peak_db, rms_db (whole), loud_db (RMS of the 20 ms windows within 30 dB of the loudest: how loud the voiced
    part is, independent of the silence around it) and length in seconds."""
    n = len(samples)
    if not n:
        return {'peak_db': MIN_DB, 'rms_db': MIN_DB, 'loud_db': MIN_DB, 'length': 0.0}
    peak = max(abs(v) for v in samples)
    ss = sum(v * v for v in samples)
    win = max(1, rate // 50)
    wins = []
    for i in range(0, n, win):
        seg = samples[i:i + win]
        wins.append(sum(v * v for v in seg) / len(seg))
    top = max(wins)
    keep = [e for e in wins if e >= top * 1e-3]
    loud = math.sqrt(sum(keep) / len(keep)) if keep else 0.0
    return {'peak_db': db(peak), 'rms_db': db(math.sqrt(ss / n)), 'loud_db': db(loud), 'length': round(n / rate, 3)}


def peaks(samples, buckets):
    """[(min, max)] per bucket, for a waveform overview."""
    n = len(samples)
    out = []
    if not n or buckets <= 0:
        return out
    step = n / buckets
    for b in range(buckets):
        seg = samples[int(b * step):max(int(b * step) + 1, int((b + 1) * step))]
        out.append((round(min(seg), 4), round(max(seg), 4)))
    return out
