"""voicecut unit tests: python -B -m unittest discover -s tools/voicecut -p "test_*.py"

Everything here is synthetic: a Vorbis stream encoded by the test itself (a forward MDCT of a known signal), a fake
PE image holding a codebook table, core files built field by field, a fake game source. No game data, no game run.
"""
import json
import math
import os
import shutil
import struct
import sys
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import vc_audio as AU  # noqa: E402
import vc_catalog as CAT  # noqa: E402
import vc_codebooks as CB  # noqa: E402
import vc_game as G  # noqa: E402
import vc_store as ST  # noqa: E402
import vc_vorbis as V  # noqa: E402
import voicecut  # noqa: E402


# ------------------------------------------------------------------ helpers

class BitWriter:
    """LSB-first, as Vorbis packs bits."""

    def __init__(self):
        self.bits = []

    def write(self, value, n):
        for i in range(n):
            self.bits.append((value >> i) & 1)

    def code(self, code, length):  # a Huffman codeword goes out MSB first
        for i in range(length - 1, -1, -1):
            self.bits.append((code >> i) & 1)

    def bytes(self):
        out = bytearray((len(self.bits) + 7) // 8)
        for i, b in enumerate(self.bits):
            out[i // 8] |= b << (i % 8)
        return bytes(out)


def float32_pack(x):
    """Vorbis float32_pack (the inverse of V.float32_unpack) for values representable with a 21-bit mantissa."""
    if x == 0:
        return 0
    sign = 0x80000000 if x < 0 else 0
    x = abs(x)
    exp = 0
    while x < (1 << 20):
        x *= 2
        exp -= 1
    while x >= (1 << 21):
        x /= 2
        exp += 1
    return sign | ((exp + 788) << 21) | int(round(x))


def lattice_book(entries, lo, hi):
    """A 1-dimensional VQ book: entry e -> lo + e * step, all codewords the same length."""
    length = entries.bit_length() - 1
    step = (hi - lo) / (entries - 1)
    return V.Codebook(1, entries, [length] * entries, 1, V.float32_unpack(float32_pack(lo)),
                      V.float32_unpack(float32_pack(step)), length, False, list(range(entries))), length


def mdct(xw):
    n = len(xw)
    return [4.0 / n * sum(xw[j] * math.cos(2 * math.pi / n * (j + 0.5 + n / 4) * (k + 0.5)) for j in range(n))
            for k in range(n // 2)]


def make_wem(signal, sizes, pow0=6, pow1=8):
    """A mono Wwise Vorbis WEM whose blocks (sizes: 2^pow0 or 2^pow1 each) encode `signal(t)`; returns
    (wem bytes, library, first output sample time, sample count). One floor per block size (flat at 1.0), one
    residue per block size quantising the MDCT coefficients on a fine lattice."""
    bs0, bs1 = 1 << pow0, 1 << pow1
    vq, vq_len = lattice_book(65536, -2.0, 2.0)
    cls = V.Codebook(1, 2, [1, 1])
    library = [cls, vq]
    # setup packet
    w = BitWriter()
    w.write(1, 8)                       # 2 codebooks
    w.write(0, 10)
    w.write(1, 10)
    w.write(1, 6)                       # 2 floors
    for p in (pow0, pow1):
        w.write(0, 5)                   # no partitions
        w.write(0, 2)                   # multiplier 1 (range 256)
        w.write(p - 1, 4)               # rangebits: X[1] = blocksize / 2
    w.write(1, 6)                       # 2 residues
    for n in (bs0, bs1):
        w.write(1, 2)                   # type 1
        w.write(0, 24)
        w.write(n // 2, 24)
        w.write(16 - 1, 24)             # partition size 16
        w.write(0, 6)                   # 1 classification
        w.write(0, 8)                   # class book 0
        w.write(1, 3)                   # cascade: pass 0 only
        w.write(0, 1)
        w.write(1, 8)                   # its book: 1 (the lattice)
    w.write(1, 6)                       # 2 mappings
    for k in (0, 1):
        w.write(0, 1)                   # one submap
        w.write(0, 1)                   # no coupling
        w.write(0, 2)
        w.write(0, 8)
        w.write(k, 8)                   # floor k
        w.write(k, 8)                   # residue k
    w.write(1, 6)                       # 2 modes
    for k in (0, 1):
        w.write(k, 1)
        w.write(k, 8)
    setup = w.bytes()
    # audio packets
    centres = []
    c = 0
    for i, n in enumerate(sizes):
        if i:
            c += sizes[i - 1] // 4 + n // 4
        centres.append(c)
    step = 4.0 / 65535
    packets = []
    for i, n in enumerate(sizes):
        pn = sizes[i - 1] if i else bs0
        nn = sizes[i + 1] if i + 1 < len(sizes) else bs0
        if n == bs0:
            pn = nn = n
        win = V.block_window(n, pn, nn)
        s0 = centres[i] - n // 2
        X = mdct([signal(s0 + j) * win[j] for j in range(n)])
        p = BitWriter()
        p.write(1 if n == bs1 else 0, 1)            # mode (Wwise strips the type and window bits)
        p.write(1, 1)                               # floor used
        p.write(255, 8)
        p.write(255, 8)                             # flat at INVERSE_DB[255] = 1.0
        for part in range(n // 2 // 16):
            p.code(0, 1)                            # class 0
            for j in range(16):
                e = max(0, min(65535, int(round((X[part * 16 + j] + 2.0) / step))))
                p.code(e, vq_len)
        packets.append(p.bytes())
    total = centres[-1] - centres[0]
    data = bytearray()
    data += struct.pack('<H', len(setup)) + setup
    audio_off = len(data)
    for pk in packets:
        data += struct.pack('<H', len(pk)) + pk
    fmt = bytearray(0x42)
    struct.pack_into('<HHIIHHH', fmt, 0, 0xFFFF, 1, 48000, 12000, 0, 0, 0x30)
    vo = 0x18
    struct.pack_into('<II', fmt, vo, total, 0xBC)
    struct.pack_into('<II', fmt, vo + 0x10, 0, audio_off)
    fmt[vo + 0x28] = pow0
    fmt[vo + 0x29] = pow1
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + bytes(fmt) + b'data' + struct.pack('<I', len(data)) + bytes(data)
    return b'RIFF' + struct.pack('<I', len(body)) + body, library, centres[0], total


def sine(t):
    return 0.4 * math.sin(2 * math.pi * 0.031 * t) + 0.15 * math.sin(2 * math.pi * 0.17 * t + 0.5)


# ------------------------------------------------------------------ Vorbis

class VorbisBasics(unittest.TestCase):
    def test_ilog_and_lookup(self):
        self.assertEqual([V.ilog(x) for x in (0, 1, 2, 3, 4, 7, 8)], [0, 1, 2, 2, 3, 3, 4])
        self.assertEqual(V.lookup1_values(81, 4), 3)
        self.assertEqual(V.lookup1_values(80, 4), 2)
        self.assertEqual(V.lookup1_values(4096, 1), 4096)

    def test_float32_roundtrip(self):
        for x in (1.0, -0.5, 0.0009765625, 123.25, -2.0):
            self.assertAlmostEqual(V.float32_unpack(float32_pack(x)), x, places=9)

    def test_huffman_spec_example(self):
        # Vorbis I spec 3.2.1: lengths 2,4,4,4,4,2,3,3 -> 00 0100 0101 0110 0111 10 110 111
        book = V.Codebook(1, 8, [2, 4, 4, 4, 4, 2, 3, 3])
        codes = {0: (0b00, 2), 1: (0b0100, 4), 2: (0b0101, 4), 3: (0b0110, 4), 4: (0b0111, 4), 5: (0b10, 2),
                 6: (0b110, 3), 7: (0b111, 3)}
        seq = [5, 0, 7, 3, 1, 6, 2, 4]
        w = BitWriter()
        for e in seq:
            w.code(*codes[e])
        br = V.BitReader(w.bytes())
        self.assertEqual([book.decode(br) for _ in seq], seq)

    def test_long_codewords_use_the_slow_path(self):
        book, length = lattice_book(4096, -1, 1)
        self.assertGreater(length, V.FAST_BITS)
        w = BitWriter()
        for e in (0, 4095, 1234):
            w.code(e, length)
        br = V.BitReader(w.bytes())
        self.assertEqual([book.decode(br) for _ in range(3)], [0, 4095, 1234])

    def test_overspecified_book_is_refused(self):
        with self.assertRaises(V.VorbisError):
            V.Codebook(1, 3, [1, 1, 1])

    def test_packed_codebook(self):
        w = BitWriter()
        w.write(2, 4)            # dims
        w.write(9, 14)           # entries
        w.write(0, 1)            # not ordered
        w.write(3, 3)            # length field: 3 bits
        w.write(1, 1)            # sparse
        lengths = [4, 0, 4, 3, 3, 4, 4, 0, 2]
        for ln in lengths:
            w.write(1 if ln else 0, 1)
            if ln:
                w.write(ln - 1, 3)
        w.write(1, 1)            # lookup type 1
        w.write(float32_pack(-1.0), 32)
        w.write(float32_pack(1.0), 32)
        w.write(1, 4)            # 2-bit values
        w.write(0, 1)
        for m in (0, 1, 2):      # lookup1_values(9, 2) = 3
            w.write(m, 2)
        book = V.parse_packed_codebook(V.BitReader(w.bytes()))
        self.assertEqual(book.lengths, lengths)
        self.assertEqual(book.vectors[0], (-1.0, -1.0))
        self.assertEqual(book.vectors[5], (1.0, 0.0))     # 5 = 2 + 1 * 3
        ordered = BitWriter()
        ordered.write(1, 4)
        ordered.write(5, 14)
        ordered.write(1, 1)      # ordered
        ordered.write(1, 5)      # first length 2
        ordered.write(1, 3)      # ilog(5) = 3 bits: one entry of length 2
        ordered.write(4, 3)      # ilog(4) = 3 bits: four of length 3
        ordered.write(0, 1)      # no lookup
        self.assertEqual(V.parse_packed_codebook(V.BitReader(ordered.bytes())).lengths, [2, 3, 3, 3, 3])

    def test_imdct_matches_the_formula(self):
        for m in (2, 8, 32, 128):
            X = [math.sin(k * 1.7) + 0.3 for k in range(m)]
            for a, b in zip(V.imdct(X), V.imdct_naive(X)):
                self.assertAlmostEqual(a, b, places=9)

    def test_window_is_power_complementary(self):
        for n in (64, 256, 2048):
            s = V.window_slope(n)
            for i in range(len(s)):
                self.assertAlmostEqual(s[i] ** 2 + s[len(s) - 1 - i] ** 2, 1.0, places=12)

    def test_render_line_matches_the_spec(self):
        v = [0] * 10
        V._render_line(0, 10, 8, 4, v, 10)   # dy -6 over 8: base 0, err += 6, a -1 step whenever err >= 8
        self.assertEqual(v[:8], [10, 10, 9, 8, 7, 7, 6, 5])

    def test_wem_header_errors(self):
        with self.assertRaises(V.VorbisError):
            V.parse_wem(b'not a wem file at all')
        wem, _lib, _c0, _n = make_wem(sine, [64, 64])
        bad = bytearray(wem)
        bad[20:22] = b'\x01\x00'                       # PCM, not Wwise Vorbis
        with self.assertRaises(V.VorbisError):
            V.parse_wem(bytes(bad))


class VorbisDecode(unittest.TestCase):
    """A stream encoded here (forward MDCT, scale 4/N, the decoder's own windows) must decode back to the signal,
    across short/long block changes: this checks the setup and packet reading, the floor, the residue, the IMDCT,
    the windows and the overlap-add together."""

    def test_round_trip_with_block_changes(self):
        sizes = [64, 64, 256, 256, 64, 256, 64, 64, 256, 256, 256, 64]
        wem, lib, c0, total = make_wem(sine, sizes)
        w, pcm = V.decode_wem(wem, lib)
        self.assertEqual(w.channels, 1)
        self.assertEqual(len(pcm[0]), total)
        err = [pcm[0][i] - sine(c0 + i) for i in range(total)]
        # what is left is the lattice's rounding (step 6e-5: max ~0.0016, and 16 times more with a 16 times coarser
        # lattice); a window, overlap or transform mistake gives errors of the signal's own size
        self.assertLess(max(abs(e) for e in err), 0.004)
        self.assertLess(math.sqrt(sum(e * e for e in err) / total), 0.0005)

    def test_skipped_channels_and_limits(self):
        wem, lib, _c0, total = make_wem(sine, [256] * 8)
        _w, pcm = V.decode_wem(wem, lib, want=[])
        self.assertEqual([len(c) for c in pcm], [0])
        seen = []
        _w, pcm = V.decode_wem(wem, lib, progress=seen.append)
        self.assertEqual(seen[-1], 1.0)
        self.assertEqual(len(pcm[0]), total)

    def test_mixed_decode_matches_the_plain_one(self):
        wem, lib, _c0, total = make_wem(sine, [64, 256, 256, 64, 256])
        _w, plain = V.decode_wem(wem, lib)
        _w, mixed = V.decode_wem(wem, lib, mix=[(0, 0.5), (7, 9.0)])    # a channel the file lacks is ignored
        self.assertEqual(len(mixed), 1)
        self.assertEqual(len(mixed[0]), total)
        self.assertLess(max(abs(a * 0.5 - b) for a, b in zip(plain[0], mixed[0])), 1e-5)
        w = V.WemInfo()
        for ch, mix in ((1, [(0, 1.0)]), (2, [(0, 0.5), (1, 0.5)]), (6, [(2, 1.0), (0, 0.5), (1, 0.5)])):
            w.channels = ch
            self.assertEqual(V.voice_mix(w), mix)

    def test_downmix(self):
        w = V.WemInfo()
        w.channels = 6
        pcm = [[1.0], [0.5], [0.25], [9.0], [9.0], [9.0]]
        self.assertEqual(V.downmix(w, pcm), [0.25 + 0.75])
        self.assertEqual(V.voice_channels(w), [0, 1, 2])


# ------------------------------------------------------------------ codebook library in a PE image

def fake_pe(books_bytes, base=0x140000000):
    """A minimal PE32+ image with one section: packed books, then a table of len(books)+1 pointers."""
    sec_raw = 0x400
    blob = bytearray()
    starts = []
    for b in books_bytes:
        starts.append(len(blob))
        blob += b
    end = len(blob)
    while len(blob) % 8:
        blob += b'\0'
    table_off = len(blob)
    va = 0x1000
    ptrs = [base + va + s for s in starts] + [base + va + end]
    blob += struct.pack('<%dQ' % len(ptrs), *ptrs)
    img = bytearray(sec_raw) + blob
    struct.pack_into('<2sH', img, 0, b'MZ', 0)
    pe = 0x80
    struct.pack_into('<I', img, 0x3C, pe)
    img[pe:pe + 4] = b'PE\0\0'
    opt_size = 0xF0
    struct.pack_into('<HHIIIHH', img, pe + 4, 0x8664, 1, 0, 0, 0, opt_size, 0x22)
    opt = pe + 24
    struct.pack_into('<H', img, opt, 0x20B)
    struct.pack_into('<Q', img, opt + 24, base)
    sec = opt + opt_size
    img[sec:sec + 8] = b'.rdata\0\0'
    struct.pack_into('<IIII', img, sec + 8, len(blob), va, len(blob), sec_raw)
    return bytes(img), sec_raw + table_off


class CodebookLibrary(unittest.TestCase):
    def book(self, k):
        w = BitWriter()
        w.write(1, 4)
        w.write(2, 14)
        w.write(0, 1)
        w.write(1, 3)
        w.write(0, 1)
        w.write(0, 1)
        w.write(0, 1)          # two entries of length 1
        w.write(0, 1)          # no lookup
        w.write(k & 0x7F, 7 + (k % 5))     # padding bits that differ per book (the parse must not depend on them)
        return w.bytes()[:4]               # 26 bits of book, then padding

    def test_finds_and_parses_the_table(self):
        books = [self.book(k) for k in range(CB.LIBRARY_POINTERS - 1)]
        img, _table = fake_pe(books)
        lib = CB.load_library(img)
        self.assertEqual(len(lib), CB.LIBRARY_POINTERS - 1)
        self.assertEqual(lib[17].lengths, [1, 1])

    def test_refuses_an_image_without_the_table(self):
        img, _ = fake_pe([self.book(k) for k in range(40)])
        with self.assertRaises(V.VorbisError):
            CB.load_library(img)


# ------------------------------------------------------------------ game data (synthetic cores)

def guid_bytes(g):
    return uuid.UUID(g).bytes_le


def hashed_string(s):
    b = s.encode()
    return struct.pack('<I', len(b)) + (struct.pack('<I', 0) + b if b else b'')


def ref(kind, g=None, path=''):
    if kind == 0:
        return b'\0'
    out = bytes([kind]) + guid_bytes(g)
    if kind >= 2:
        out += hashed_string(path)
    return out


def obj(tid, body):
    return struct.pack('<QI', tid, len(body)) + body


def sentence_body(g, sound, text, voice_path):
    return (guid_bytes(g) + bytes([1]) + struct.pack('<f', 0.5) + b'\x01' + ref(1, sound) + ref(0) +
            (ref(1, text) if text else ref(0)) + ref(2, '11111111-2222-3333-4444-555555555555', voice_path))


def sound_body(g, stream_base, length):
    lengths = [0.0] + [length] * 25
    wems = [0] + [1000 + i for i in range(25)]
    body = (guid_bytes(g) + struct.pack('<I', 0) + ref(2, '99999999-0000-0000-0000-000000000000', 'ds/sounds/grp') +
            b'\x07' * 132 + struct.pack('<I', 454841570) + b'\x00' + ref(0) +
            ref(2, '88888888-0000-0000-0000-000000000000', 'ds/sounds/preset') +
            struct.pack('<I', 26) + struct.pack('<26f', *lengths) + struct.pack('<I', 26) + struct.pack('<26I', *wems) +
            struct.pack('<f', 0.0))
    tail = b'\xff\x0f\x01\x01\x02\x20'
    for lang in ('english', 'french'):
        p = (stream_base + '.wem.' + lang).encode()
        tail += b'\xa3' + struct.pack('<I', len(p)) + p + b'h' + b'\x00' * 15 + struct.pack('<QI', 27, 4000)
    return body + tail


def text_body(g, english):
    out = guid_bytes(g)
    for i in range(25):
        t = (english if i == 0 else ('fr ' + english if i == 1 else '')).encode()
        out += struct.pack('<H', len(t)) + t + struct.pack('<H', 0) + b'\0'
    return out


def sentences_core(lines):
    """lines: (sentence guid, voice, english text or '', length or None)"""
    out = b''
    for k, (g, voice, text, length) in enumerate(lines):
        sg = '%08x-0000-0000-0000-00000000000a' % k
        tg = '%08x-0000-0000-0000-00000000000b' % k
        out += obj(G.TYPE_SENTENCE, sentence_body(g, sg, tg if text else None, 'localized/voices/' + voice))
        if length is not None:
            out += obj(G.TYPE_SIMPLE_SOUND, sound_body(sg, 'localized/sentences/x/sentences_sentence_' + g, length))
        if text:
            out += obj(G.TYPE_TEXT, text_body(tg, text))
    return out


SAM_A = 'aaaaaaaa-0000-0000-0000-000000000001'
SAM_B = 'aaaaaaaa-0000-0000-0000-000000000002'
FRG_A = 'bbbbbbbb-0000-0000-0000-000000000001'


def resident_core():
    out = b''
    players = [('cccccccc-0000-0000-0000-000000000001', SAM_A), ('cccccccc-0000-0000-0000-000000000002', SAM_B)]
    for pg, sg in players:
        out += obj(G.TYPE_PLAYER_SENTENCE, guid_bytes(pg) + ref(2, sg, 'localized/sentences/ds_lines_sam/lines_sam/sentences') +
                   struct.pack('<Qi', 0, -1) + b'\x00\x01\x00')
    for k, (h, pl) in enumerate([(G.situation_hash('Jumping'), [players[0][0]]), (0x1234567, [players[1][0]])]):
        body = guid_bytes('dddddddd-0000-0000-0000-%012d' % k) + struct.pack('<I', len(pl))
        for p in pl:
            body += ref(1, p)
        out += obj(G.TYPE_SITUATION, body + struct.pack('<I', h))
    return out


def priority_core():
    body = guid_bytes('eeeeeeee-0000-0000-0000-000000000001') + struct.pack('<IiI', G.situation_hash('Jumping'), 30, 3)
    body += b'\0' * 6 + b'\x00\x00\x01'
    return obj(G.TYPE_PRIORITY_INFO, body)


def cutscene_core(seq):
    body = b'\x01' + guid_bytes('ffffffff-0000-0000-0000-000000000001') + struct.pack('<II', 7, 1000) + struct.pack('<I', 0) + b'\x01\x01'
    p = ('ds/sounds/wwise_cinematics_sound_resource/cs03/%s/wav/english/windows/%s_voice_track.english' % (seq, seq)).encode()
    body += b'\x4f\x3a\x8d\x43' + struct.pack('<I', len(p)) + p + b'r' * 16
    return obj(G.TYPE_WEM_LOCALIZED, body)


CUT_FRG = 'bbbbbbbb-0000-0000-0000-000000000002'
CUT_SAM = 'bbbbbbbb-0000-0000-0000-000000000003'
CUT_DIE = 'bbbbbbbb-0000-0000-0000-000000000004'
CUT_FRG_ELSEWHERE = 'bbbbbbbb-0000-0000-0000-000000000005'


def subtitle_event(g, start, end, sentence, enabled=True):
    body = guid_bytes(g) + struct.pack('<i', start) + bytes([1 if enabled else 0]) + struct.pack('<i', end)
    body += ref(2, sentence, 'localized/sentences/ds_lines_cutscene/x/sentences') + b'\x00' + struct.pack('<ii', 5, 10)
    return obj(G.TYPE_SUBTITLE_EVENT, body)


def speech_core(events):
    return b''.join(subtitle_event('cccccccc-1111-0000-0000-%012d' % k, a, b, s) for k, (a, b, s) in enumerate(events))


def speech_path(seq):
    return G.CINEMATIC_SPEECH.format(cs='cs03', seq=seq, flat=seq.replace('_', ''))


class FakeGameData:
    """sq_cs03_s00200: Fragile, Sam and another voice, timed; sq_cs03_s00210: a timeline only, reusing her line kept in
    s00200's file; sq_cs03_s00300: her text in its sentence file but not in its own timeline (spoken elsewhere)."""

    def __init__(self, game_dir):
        self.game_dir = game_dir
        seq = 'sq_cs03_s00200'
        self.files = {
            'localized/sentences/ds_lines_sam/lines_sam/sentences': sentences_core(
                [(SAM_A, 'vr0010_sam', '', 0.7), (SAM_B, 'vr0010_sam', 'Almost there.', 1.2)]),
            'localized/sentences/ds_lines_terminal/lines_01/sentences': sentences_core(
                [(FRG_A, 'vr0040_frg', 'Sam, it\'s Fragile.', 1.5),
                 ('bbbbbbbb-0000-0000-0000-000000000009', 'vr920_prp', 'Not ours.', 1.0)]),
            'localized/sentences/ds_lines_cutscene/%s/sentences' % seq: sentences_core(
                [(CUT_FRG, 'vr0040_frg', 'Got a delivery for a porter.', None),
                 (CUT_SAM, 'vr0010_sam', 'Why are you here?', None), (CUT_DIE, 'vr0100_die', 'Sam.', None)]),
            'localized/sentences/ds_lines_cutscene/sq_cs03_s00300/sentences': sentences_core(
                [(CUT_FRG_ELSEWHERE, 'vr0040_frg', 'Not in this one.', None)]),
            G.SAM_RESIDENT: resident_core(), G.SAM_PRIORITY: priority_core(),
            G.CINEMATIC_SOUND.format(cs='cs03', seq=seq): cutscene_core(seq),
            speech_path(seq): speech_core([(300, 360, CUT_DIE), (60, 110, CUT_SAM), (120, 240, CUT_FRG)]),
            G.CINEMATIC_SOUND.format(cs='cs03', seq='sq_cs03_s00210'): cutscene_core('sq_cs03_s00210'),
            speech_path('sq_cs03_s00210'): speech_core([(600, 660, CUT_FRG)]),
            G.CINEMATIC_SOUND.format(cs='cs03', seq='sq_cs03_s00300'): cutscene_core('sq_cs03_s00300'),
            speech_path('sq_cs03_s00300'): speech_core([(10, 20, CUT_DIE)]),
        }

    def index(self):
        return {}

    def all_paths(self):
        return list(self.files) + ['ds/other/thing']

    def read(self, path):
        if path not in self.files:
            raise KeyError(path)
        return self.files[path]


class GameParsing(unittest.TestCase):
    def test_subtitle_events(self):
        start, end, g, on = G.parse_subtitle_event(subtitle_event('cccccccc-0000-0000-0000-000000000001', 12047, 12178,
                                                                  CUT_FRG)[12:])
        self.assertEqual((start, end, g, on), (12047, 12178, CUT_FRG, True))
        core = speech_core([(120, 240, CUT_FRG)]) + subtitle_event('cccccccc-0000-0000-0000-000000000009', 1, 2, CUT_SAM,
                                                                     enabled=False)
        subs = G.cutscene_subtitles(core, {CUT_FRG: ('vr0040_frg', 'Hi')})
        self.assertEqual(subs, [{'t0': 2.0, 't1': 4.0, 'who': 'fragile', 'text': 'Hi'}])   # the disabled one is out
        voices = G.sentence_voices(FakeGameData('x').files['localized/sentences/ds_lines_cutscene/sq_cs03_s00200/sentences'])
        self.assertEqual(voices[CUT_SAM], ('vr0010_sam', 'Why are you here?'))

    def test_situation_hash(self):
        # docs/audio-research.md 12.1: Jumping 0x2CF2EAD9, OutOfBreath 0x36B74D25
        self.assertEqual(G.situation_hash('Jumping'), 0x2CF2EAD9)
        self.assertEqual(G.situation_hash('OutOfBreath'), 0x36B74D25)

    def test_stream_paths_with_letters_after_the_name(self):
        body = sound_body('aaaaaaaa-0000-0000-0000-000000000000', 'localized/a/b_sentence_x', 1.0)
        s = G.parse_simple_sound(body)
        self.assertEqual(s['streams']['english'], 'localized/a/b_sentence_x.wem.english')
        self.assertEqual(s['streams']['french'], 'localized/a/b_sentence_x.wem.french')
        self.assertAlmostEqual(s['lengths'][1], 1.0)
        self.assertEqual(s['cookie'], 454841570)

    def test_sentence_text_and_core_scan(self):
        core = FakeGameData('x').files['localized/sentences/ds_lines_terminal/lines_01/sentences']
        lines = G.scan_sentence_core(core, 'localized/sentences/ds_lines_terminal/lines_01/sentences')
        self.assertEqual(len(lines), 1)            # the other voice is not wanted
        ln = lines[0]
        self.assertEqual((ln['speaker'], ln['text'], ln['length']), ('fragile', 'Sam, it\'s Fragile.', 1.5))
        self.assertTrue(ln['stream'].endswith(FRG_A + '.wem.english'))
        fr = G.scan_sentence_core(core, 'p', language='french')[0]
        self.assertEqual(fr['text'], 'fr Sam, it\'s Fragile.')
        self.assertTrue(fr['stream'].endswith('.wem.french'))

    def test_bad_core_is_refused(self):
        with self.assertRaises(G.FormatError):
            G.read_objects(obj(1, b'abc') + b'xx')
        with self.assertRaises(G.FormatError):
            G.parse_sentence(sentence_body(SAM_A, SAM_B, None, 'v') + b'junk')

    def test_sam_situations(self):
        by_sentence, prio, flags, hashes = G.scan_sam_situations(resident_core(), priority_core())
        j = G.situation_hash('Jumping')
        self.assertEqual(by_sentence[SAM_A], [j])
        self.assertEqual(by_sentence[SAM_B], [0x1234567])
        self.assertEqual(prio[j]['priority'], 30)
        self.assertTrue(prio[j]['reaction'])
        self.assertEqual(flags[j]['tired'], 1)
        self.assertEqual(hashes, [j, 0x1234567])

    def test_cutscene_of(self):
        self.assertEqual(G.cutscene_of('localized/sentences/ds_lines_cutscene/sq_cs05_s00916/sentences'),
                         ('cs05', 'sq_cs05_s00916'))
        self.assertIsNone(G.cutscene_of('localized/sentences/ds_lines_sam/lines_sam/sentences'))


class Catalog(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='voicecut-test-')
        with open(os.path.join(self.tmp, 'ds.exe'), 'wb') as f:
            f.write(b'MZ')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_build(self):
        cat = CAT.build_catalog(FakeGameData(self.tmp))
        by = {i['id']: i for i in cat['items']}
        # s00300 is left out: its own timeline does not have her (her line there is spoken in another cutscene)
        self.assertEqual(set(by), {'s-' + SAM_A, 's-' + SAM_B, 'f-' + FRG_A, 'c-sq_cs03_s00200', 'c-sq_cs03_s00210'})
        subs = by['c-sq_cs03_s00200']['subs']
        self.assertEqual([(s['t0'], s['t1'], s['who']) for s in subs],
                         [(1.0, round(110 / 60, 3), 'sam'), (2.0, 4.0, 'fragile'), (5.0, 6.0, 'other')])
        self.assertEqual(subs[1]['text'], 'Got a delivery for a porter.')
        self.assertEqual([(s['t0'], s['who'], s['text']) for s in by['c-sq_cs03_s00210']['subs']],
                         [(10.0, 'fragile', 'Got a delivery for a porter.')])
        self.assertEqual(by['c-sq_cs03_s00210']['lines'], 1)
        self.assertEqual(by['s-' + SAM_A]['category'], 'jump')
        self.assertTrue(by['s-' + SAM_A]['sure'])
        self.assertEqual(by['s-' + SAM_B]['category'], 'random_line')      # unknown name, spoken: a guess
        self.assertFalse(by['s-' + SAM_B]['sure'])
        cut = by['c-sq_cs03_s00200']
        self.assertEqual((cut['kind'], cut['speaker'], cut['lines']), ('cutscene', 'fragile', 1))
        self.assertTrue(cut['stream'].endswith('sq_cs03_s00200_voice_track.english'))
        sits = {s['hash']: s for s in cat['situations']}
        self.assertEqual(sits[G.situation_hash('Jumping')]['label'], 'Jumping')
        self.assertEqual(sits[0x1234567]['label'], 'situation 01234567')
        path = os.path.join(self.tmp, 'cat.json')
        CAT.save_catalog(path, cat)
        self.assertIsNotNone(CAT.load_catalog(path, cat['game'], 'english'))
        self.assertIsNone(CAT.load_catalog(path, 'other-game', 'english'))
        self.assertIsNone(CAT.load_catalog(path, cat['game'], 'french'))

    def test_every_known_name_has_a_category(self):
        for name, cat in CAT.KNOWN_SITUATIONS.items():
            self.assertIn(cat, CAT.CATEGORY_KEYS, name)


# ------------------------------------------------------------------ audio, store, export

class Audio(unittest.TestCase):
    def test_render(self):
        src = [0.5] * 4800
        self.assertEqual(AU.render(src, 48000, {'start': 0.2, 'end': 0.1}), [])
        out = AU.render(src, 48000, {'start': 0.01, 'end': 0.06, 'gain_db': 6.0206})
        self.assertEqual(len(out), 2400)
        self.assertAlmostEqual(out[1000], 1.0, places=3)
        out = AU.render(src, 48000, {'start': 0, 'end': 0.1, 'normalize': True})
        self.assertAlmostEqual(max(out), 10 ** (-3 / 20), places=6)
        out = AU.render(src, 48000, {'start': 0, 'end': 0.1, 'fade_in': 0.01, 'fade_out': 0.01})
        self.assertLess(out[0], 0.01)
        self.assertLess(out[-1], 0.01)
        self.assertAlmostEqual(out[2400], 0.5)
        self.assertAlmostEqual(out[240], 0.25, places=2)                 # half way up the raised cosine

    def test_stats_and_wav_round_trip(self):
        x = [0.0] * 4800 + [0.5 * math.sin(i / 7) for i in range(4800)]
        st = AU.stats(x)
        self.assertAlmostEqual(st['peak_db'], -6.02, places=1)
        self.assertAlmostEqual(st['loud_db'], -9.03, places=0)            # the silent half does not count
        self.assertLess(st['rms_db'], st['loud_db'])
        rate, y = AU.read_wav(AU.wav_bytes(x))
        self.assertEqual((rate, len(y)), (48000, len(x)))
        self.assertLess(max(abs(a - b) for a, b in zip(x, y)), 1e-4)


    def test_read_wav_range(self):
        tmp = tempfile.mkdtemp(prefix='voicecut-test-')
        try:
            p = os.path.join(tmp, 'x.wav')
            x = [((i % 200) - 100) / 200.0 for i in range(48000)]
            AU.write_wav(p, x)
            self.assertEqual(AU.wav_frames(p), (48000, 48000))
            rate, seg, frames = AU.read_wav_range(p, 0.25, 0.5)
            self.assertEqual((rate, len(seg), frames), (48000, 12000, 48000))
            self.assertLess(max(abs(a - b) for a, b in zip(seg, x[12000:24000])), 1e-4)
            self.assertEqual(len(AU.read_wav_range(p, 0.9, 5.0)[1]), 4800)     # clamped at the end
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class Store(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='voicecut-test-')
        self.ws = ST.Workspace(os.path.join(self.tmp, 'ws'), os.path.join(self.tmp, 'export'))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_clip_checks(self):
        ok = {'source': 'f-abc', 'category': 'stumble', 'start': 0.1, 'end': 0.5}
        self.assertEqual(ST.clean_clip(ok)['category'], 'stumble')
        for bad in ({'category': 'nope'}, {'source': '../x'}, {'end': 0.11}, {'end': 99}, {'gain_db': 'loud'},
                    {'gain_db': float('nan')}, {'id': 'clip-zz'}):
            with self.assertRaises(ST.ClipError, msg=str(bad)):
                ST.clean_clip(dict(ok, **bad))
        with self.assertRaises(ST.ClipError):
            ST.clean_clip(ok, known_sources={'f-other'})

    def test_upsert_delete_situations_persist(self):
        c = self.ws.upsert_clip({'source': 'f-a', 'category': 'pain', 'start': 0, 'end': 1})
        self.assertRegex(c['id'], ST.CLIP_ID)
        c2 = self.ws.upsert_clip(dict(c, category='fall'))
        self.assertEqual((c2['id'], c2['category']), (c['id'], 'fall'))
        with self.assertRaises(ST.ClipError):
            self.ws.upsert_clip(dict(c, id='clip-00000000'))
        self.ws.set_situation(123, 'stumble')
        again = ST.Workspace(self.ws.root, self.ws.export_dir)
        self.assertEqual(again.clips()[0]['category'], 'fall')
        self.assertEqual(again.state['situations'], {'123': 'stumble'})
        again.set_situation(123, None)
        self.assertEqual(again.state['situations'], {})
        again.delete_clip(c['id'])
        self.assertEqual(again.clips(), [])
        with self.assertRaises(ST.ClipError):
            again.delete_clip(c['id'])

    @staticmethod
    def ranged(src, rate=48000):
        def load(_item, a, b):
            return rate, src[int(round(a * rate)):int(round(b * rate))], len(src)
        return load

    def test_export_replaces_only_its_own_files(self):
        src = [0.25 * math.sin(i / 5) for i in range(48000)]
        for cat, a in (('stumble', 0.1), ('stumble', 0.3), ('pain', 0.5)):
            self.ws.upsert_clip({'source': 'f-a', 'category': cat, 'start': a, 'end': a + 0.2})
        os.makedirs(os.path.join(self.ws.export_dir, 'fragile'), exist_ok=True)
        mine = os.path.join(self.ws.export_dir, 'fragile', 'my-notes.txt')
        with open(mine, 'w') as f:
            f.write('keep me')
        r = self.ws.export(self.ranged(src), game='1-2')
        self.assertEqual((r['clips'], r['by_category'], r['errors']), (3, {'stumble': 2, 'pain': 1}, []))
        rows = ST.read_manifest(r['manifest'])
        self.assertEqual([x['file'] for x in rows],
                         [os.path.join('fragile', 'stumble', 'stumble_01.wav'), os.path.join('fragile', 'stumble', 'stumble_02.wav'),
                          os.path.join('fragile', 'pain', 'pain_01.wav')])
        self.assertEqual(rows[0]['samples'], 9600)
        rate, x = AU.read_wav(os.path.join(self.ws.export_dir, rows[0]['file']))
        self.assertEqual((rate, len(x)), (48000, 9600))
        with open(r['manifest'], encoding='utf-8') as f:
            head = f.read().split('\n')
        self.assertEqual(head[0], 'stv-voices 1')
        self.assertTrue(head[1].startswith('game\t1-2\t'))
        # a second export after deleting a clip removes the stale file it wrote, never the user's own
        self.ws.delete_clip(self.ws.clips()[2]['id'])
        self.ws.export(self.ranged(src))
        self.assertFalse(os.path.exists(os.path.join(self.ws.export_dir, 'fragile', 'pain', 'pain_01.wav')))
        self.assertTrue(os.path.exists(mine))
        # a broken source is skipped with a reason
        r = self.ws.export(lambda i, a, b: (_ for _ in ()).throw(KeyError(i)))
        self.assertEqual(r['clips'], 0)
        self.assertEqual(len(r['errors']), 2)

    def test_plans_links_and_their_manifest_columns(self):
        with self.assertRaises(ST.ClipError):
            ST.clean_clip({'source': 'f-a', 'category': 'pain', 'start': 0, 'end': 1, 'situations': [-1]})
        with self.assertRaises(ST.ClipError):
            ST.clean_clip({'source': 'f-a', 'category': 'pain', 'start': 0, 'end': 1, 'situations': 'x'})
        c = self.ws.upsert_clip({'source': 'f-a', 'category': 'pain', 'start': 0, 'end': 0.5, 'situations': [7, 7, 3]})
        self.assertEqual(c['situations'], [3, 7])
        self.assertEqual(self.ws.link_clip(c['id'], 9)['situations'], [3, 7, 9])
        self.assertEqual(self.ws.link_clip(c['id'], 3, on=False)['situations'], [7, 9])
        with self.assertRaises(ST.ClipError):
            self.ws.link_clip('clip-00000000', 3)
        self.ws.set_plan(11, 'silent')
        self.ws.set_plan(12, 'any')
        with self.assertRaises(ST.ClipError):
            self.ws.set_plan(12, 'loud')
        self.ws.set_plan(12, None)
        self.assertEqual(ST.Workspace(self.ws.root, self.ws.export_dir).state['plan'], {'11': 'silent'})
        src = [0.1] * 48000
        r = self.ws.export(Store.ranged(src), situations=[(9, 'pain', 'guess', 'situation 00000009', 'clip'),
                                                         (11, 'other', 'guess', 'situation 0000000B', 'silent'),
                                                         (13, 'other', 'guess', 'situation 0000000D', '-')])
        with open(r['manifest'], encoding='utf-8') as f:
            text = f.read()
        clip_row = [ln for ln in text.split('\n') if ln.startswith('clip\t')][0].split('\t')
        self.assertEqual(clip_row[-1], '00000007,00000009')
        self.assertIn('situation\t00000009\tpain\tguess\tsituation 00000009\tclip', text)
        self.assertIn('situation\t0000000B\tother\tguess\tsituation 0000000B\tsilent', text)   # kept: it has a plan
        self.assertNotIn('0000000D', text)
        self.assertEqual(r['situations'], 2)

    def test_manifest_refuses_paths_outside(self):
        p = os.path.join(self.tmp, 'voices.txt')
        with open(p, 'w', encoding='utf-8') as f:
            f.write('stv-voices 1\nclip\t..\\..\\evil.wav' + '\tx' * 12 + '\nclip\tC:\\evil.wav' + '\tx' * 12 + '\n')
        self.assertEqual(ST.read_manifest(p), [])


# ------------------------------------------------------------------ the API (in process and over HTTP)

class FakeSource:
    def __init__(self, catalog):
        self.catalog = catalog
        self.decodes = 0

    def stamp(self):
        return '1-2'

    def build_catalog(self, language, progress):
        progress('Done', 1, 1)
        return self.catalog

    def decode(self, item, progress=None):
        self.decodes += 1
        if progress:
            progress(0.5)
        return 48000, [0.3 * math.sin(i / 9) for i in range(48000)]

    # the background decode, with threads instead of processes
    def prefetch_pool(self, workers):
        from concurrent.futures import ThreadPoolExecutor
        return ThreadPoolExecutor(max_workers=workers)

    def prefetch_submit(self, pool, item, path):
        import vc_worker
        return pool.submit(lambda: vc_worker.write_cache(path, *self.decode(item)))


def fake_catalog():
    return {'version': CAT.CATALOG_VERSION, 'language': 'english', 'game': '1-2', 'items': [
        {'id': 'f-' + FRG_A, 'kind': 'line', 'speaker': 'fragile', 'text': 'Hi', 'length': 1.0, 'stream': 's1',
         'group': 'terminal', 'situations': [], 'category': 'random_line', 'sure': False},
        {'id': 's-' + SAM_A, 'kind': 'line', 'speaker': 'sam', 'text': '', 'length': 1.0, 'stream': 's2',
         'group': 'sam', 'situations': [5], 'category': 'jump', 'sure': True}],
        'situations': [{'hash': 5, 'label': 'situation 00000005', 'guess': 'other', 'lines': 1, 'spoken': 0}],
        'categories': [{'key': k, 'label': l, 'hint': h} for k, l, h in CAT.CATEGORIES]}


class Api(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='voicecut-test-')
        self.ws = ST.Workspace(os.path.join(self.tmp, 'ws'), os.path.join(self.tmp, 'export'))
        self.src = FakeSource(fake_catalog())
        self.app = voicecut.App(self.ws, self.src)
        self.app.start_catalog()
        for _ in range(200):
            if self.app.cat_state['state'] == 'ready':
                break
            threading.Event().wait(0.01)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_no_game(self):
        app = voicecut.App(self.ws, None)
        app.start_catalog()
        self.assertEqual(app.cat_state['state'], 'error')
        self.assertFalse(app.route('GET', '/api/catalog', {})[1]['ready'])

    def test_flow(self):
        code, cat = self.app.route('GET', '/api/catalog', {})
        self.assertTrue(cat['ready'])
        self.assertNotIn('stream', cat['items'][0])
        code, wav, ctype, _ = self.app.route('GET', '/api/audio', {'id': ['f-' + FRG_A]})
        self.assertEqual((code, ctype, wav[:4]), (200, 'audio/wav', b'RIFF'))
        self.app.route('GET', '/api/audio', {'id': ['f-' + FRG_A]})
        self.assertEqual(self.src.decodes, 1)                       # cached
        self.assertEqual(self.app.route('GET', '/api/audio', {'id': ['x-nope']})[0], 404)
        code, c = self.app.route('POST', '/api/clip', None, {'source': 'f-' + FRG_A, 'category': 'effort',
                                                             'start': 0.1, 'end': 0.4, 'normalize': True})
        self.assertEqual(code, 200)
        self.assertAlmostEqual(c['stats']['peak_db'], -3.0, places=1)
        self.assertEqual(self.app.route('POST', '/api/clip', None, {'source': 'f-' + FRG_A, 'category': 'effort',
                                                                    'start': 0.5, 'end': 1.5})[0], 400)
        self.assertEqual(self.app.route('POST', '/api/clip', None, {'source': 'f-nope', 'category': 'effort',
                                                                    'start': 0.1, 'end': 0.5})[0], 400)
        self.assertEqual(self.app.route('POST', '/api/situation', None, {'hash': 5, 'category': 'fall'})[0], 200)
        self.assertEqual(self.app.route('GET', '/api/user', {})[1]['situations'], {'5': 'fall'})
        code, r = self.app.route('POST', '/api/export', None, {})
        self.assertEqual((code, r['clips'], r['situations']), (200, 1, 1))
        with open(r['manifest'], encoding='utf-8') as f:
            self.assertIn('situation\t00000005\tfall\tuser\tsituation 00000005', f.read())
        self.assertEqual(self.app.route('POST', '/api/open', None, {'what': 'C:\\Windows'})[0], 400)

    def wait(self, cond, n=500):
        for _ in range(n):
            if cond():
                return True
            threading.Event().wait(0.01)
        return cond()

    def test_prefetch_decodes_everything_once(self):
        app = voicecut.App(self.ws, self.src, prefetch=True, workers=2)
        app.start_catalog()
        self.assertTrue(self.wait(lambda: app.prefetch['state'] == 'done'))
        self.assertEqual((app.prefetch['done'], app.prefetch['total'], app.prefetch['failed']), (2, 2, 0))
        for i in ('f-' + FRG_A, 's-' + SAM_A):
            self.assertTrue(os.path.exists(self.ws.audio_path(i)))
            self.assertEqual(self.ws.measured[i]['length'], 1.0)
        n = self.src.decodes
        self.app.route('GET', '/api/audio', {'id': ['s-' + SAM_A]})        # served from the cache
        self.assertEqual(self.src.decodes, n)
        with open(self.ws.measure_path, encoding='utf-8') as f:            # flushed at the end
            self.assertEqual(len(json.load(f)), 2)
        again = voicecut.App(self.ws, self.src, prefetch=True)
        again.start_catalog()
        self.assertEqual(again.prefetch['state'], 'done')                  # nothing left to do
        self.assertEqual(self.app.route('POST', '/api/prefetch', None, {'action': 'go'})[0], 400)

    def test_prefetch_pause(self):
        app = voicecut.App(self.ws, self.src, prefetch=False, workers=1)
        app.start_catalog()
        self.assertTrue(self.wait(lambda: app.cat_state['state'] == 'ready'))
        app._prefetch_paused.set()
        app.start_prefetch()                                               # starts, then resumes (clears the pause)
        self.assertTrue(self.wait(lambda: app.prefetch['state'] == 'done'))
        app.pause_prefetch()                                               # nothing running: no change
        self.assertEqual(app.prefetch['state'], 'done')

    def test_guide_routes(self):
        code, c = self.app.route('POST', '/api/clip', None, {'source': 'f-' + FRG_A, 'category': 'jump',
                                                             'start': 0.1, 'end': 0.4, 'situations': [5]})
        self.assertEqual((code, c['situations']), (200, [5]))
        self.assertEqual(self.app.route('POST', '/api/clip/link', None, {'id': c['id'], 'hash': 6})[1]['situations'],
                         [5, 6])
        self.assertEqual(self.app.route('POST', '/api/clip/link', None, {'id': 'clip-00000000', 'hash': 6})[0], 400)
        self.assertEqual(self.app.route('POST', '/api/plan', None, {'hash': 6, 'plan': 'silent'})[1]['plan'],
                         {'6': 'silent'})
        self.assertEqual(self.app.route('POST', '/api/plan', None, {'hash': 6, 'plan': 'x'})[0], 400)
        self.assertEqual(self.app.route('GET', '/api/user', {})[1]['plan'], {'6': 'silent'})
        rows = {r[0]: r for r in self.app.situation_rows()}
        self.assertEqual(rows[5][4], 'clip')

    def test_measure(self):
        self.assertEqual(self.app.route('POST', '/api/measure', None, {'ids': 'x'})[0], 400)
        self.app.route('POST', '/api/measure', None, {'ids': ['s-' + SAM_A, 'x-unknown']})
        for _ in range(300):
            if 's-' + SAM_A in self.ws.measured and not self.app._measure_queue:
                break
            threading.Event().wait(0.01)
        self.assertIn('s-' + SAM_A, self.ws.measured)
        self.assertEqual(self.app.status()['measuring'], 0)

    def test_http_guards(self):
        voicecut.Handler.app = self.app
        server = voicecut.Server(('127.0.0.1', 0), voicecut.Handler)
        port = server.server_address[1]
        voicecut.Handler.port = port
        t = threading.Thread(target=server.serve_forever, daemon=True)
        t.start()
        try:
            base = 'http://127.0.0.1:%d' % port

            def req(path, body=None, headers=None):
                r = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(),
                                           headers=headers or {}, method='POST' if body is not None else 'GET')
                try:
                    with urllib.request.urlopen(r, timeout=5) as resp:
                        return resp.status, resp.read()
                except urllib.error.HTTPError as e:
                    with e:
                        return e.code, e.read()
            self.assertEqual(json.loads(req('/api/ping')[1])['app'], 'voicecut')
            self.assertEqual(req('/api/ping', headers={'Host': 'evil.example:%d' % port})[0], 403)
            body = {'source': 'f-' + FRG_A, 'category': 'pain', 'start': 0, 'end': 0.5}
            self.assertEqual(req('/api/clip', body, {'Content-Type': 'application/json'})[0], 403)
            self.assertEqual(req('/api/clip', body, {'X-Voicecut': '1', 'Content-Type': 'text/plain'})[0], 403)
            code, out = req('/api/clip', body, {'X-Voicecut': '1', 'Content-Type': 'application/json'})
            self.assertEqual(code, 200, out)
            code, page = req('/')
            self.assertEqual(code, 200)
            self.assertIn(b'voicecut', page)
        finally:
            server.shutdown()
            server.server_close()


class Files(unittest.TestCase):
    def test_static_files_exist_and_bat_example(self):
        for _, (name, _ctype) in voicecut.STATIC_FILES.items():
            self.assertTrue(os.path.isfile(os.path.join(voicecut.STATIC, name)), name)
        with open(os.path.join(HERE, 'voicecut.bat.example'), 'rb') as f:
            bat = f.read()
        self.assertIn(b'\r\n', bat)
        self.assertIn(b'tools\\voicecut\\voicecut.py', bat)
        self.assertNotIn(b'devdash', bat)


if __name__ == '__main__':
    unittest.main()
