"""voicecut: a minimal Wwise Vorbis (.wem, codec 0xFFFF) decoder. Python 3 standard library only.

Death Stranding Director's Cut stores voice audio as Wwise Vorbis WEM files (RIFF/WAVE, format tag 0xFFFF, a
0x42-byte "fmt " chunk holding the "vorb" header, modern 2-byte packet headers, stripped packet-type and window
bits, and codebooks referenced by id from Wwise's packed aoTuV 6.03 codebook library). This decodes that to float
PCM. The format notes follow the public descriptions of ww2ogg (hcs) and the Vorbis I specification; the code is
written from the specification, not copied. The codebook library is not shipped: vc_codebooks reads it from the
user's own ds.exe (Wwise is linked into the game) at run time.

  wem = parse_wem(data)                    # header fields (WemInfo)
  pcm = decode_wem(data, codebooks)        # list of channels, each a list of floats (nominally -1..1)

Nothing here reads files; callers pass bytes.
"""
import array
import cmath
import math
import struct


class VorbisError(ValueError):
    pass


def ilog(x):
    """Vorbis ilog: the number of bits needed to hold x (ilog(0) = 0)."""
    return x.bit_length() if x > 0 else 0


def float32_unpack(x):
    mantissa = x & 0x1FFFFF
    sign = x & 0x80000000
    exponent = (x & 0x7FE00000) >> 21
    v = float(mantissa)
    if sign:
        v = -v
    return math.ldexp(v, exponent - 788)


def lookup1_values(entries, dims):
    """The largest r with r ** dims <= entries."""
    r = int(math.floor(entries ** (1.0 / dims))) if entries > 0 else 0
    while (r + 1) ** dims <= entries:
        r += 1
    while r > 0 and r ** dims > entries:
        r -= 1
    return r


class BitReader:
    """LSB-first bit reader over bytes (the Vorbis bit order)."""

    __slots__ = ('val', 'pos', 'nbits')

    def __init__(self, data, start_bit=0):
        self.val = int.from_bytes(bytes(data), 'little')
        self.pos = start_bit
        self.nbits = len(data) * 8

    def read(self, n):
        if n == 0:
            return 0
        if self.pos + n > self.nbits:
            raise VorbisError('read past the end of a packet')
        v = (self.val >> self.pos) & ((1 << n) - 1)
        self.pos += n
        return v

    def peek(self, n):
        return (self.val >> self.pos) & ((1 << n) - 1)

    def left(self):
        return self.nbits - self.pos


# ------------------------------------------------------------------ codebooks

FAST_BITS = 10


class Codebook:
    """One Vorbis codebook: entry lengths, canonical codewords (LSB-first decode tables) and VQ vectors."""

    def __init__(self, dims, entries, lengths, lookup_type=0, minimum=0.0, delta=0.0, value_bits=0,
                 sequence_p=False, multiplicands=()):
        self.dims = dims
        self.entries = entries
        self.lengths = lengths
        self.lookup_type = lookup_type
        self._build_huffman()
        self.vectors = None
        if lookup_type == 1:
            self._build_vq(minimum, delta, sequence_p, multiplicands)

    def _build_huffman(self):
        lengths = self.lengths
        used = [(i, l) for i, l in enumerate(lengths) if l > 0]
        self.single = None
        if len(used) == 1:
            self.single = used[0][0]   # one used entry: libvorbis reads its length's bits and returns it
            self.single_len = used[0][1]
            self.fast = None
            self.slow = {}
            self.maxlen = used[0][1]
            return
        marker = [0] * 33
        codes = {}
        for i, length in used:
            entry = marker[length]
            if length < 32 and (entry >> length):
                raise VorbisError('codebook is over-specified')
            codes[i] = entry
            for j in range(length, 0, -1):
                if marker[j] & 1:
                    if j == 1:
                        marker[1] += 1
                    else:
                        marker[j] = marker[j - 1] << 1
                    break
                marker[j] += 1
            for j in range(length + 1, 33):
                if (marker[j] >> 1) == entry:
                    entry = marker[j]
                    marker[j] = marker[j - 1] << 1
                else:
                    break
        self.maxlen = max((l for _, l in used), default=0)
        fast = [None] * (1 << FAST_BITS)
        slow = {}
        for i, length in used:
            c = codes[i]
            rev = int(format(c, '0%db' % length)[::-1], 2)  # the bitstream sends the MSB first, read LSB-first
            if length <= FAST_BITS:
                step = 1 << length
                for k in range(rev, 1 << FAST_BITS, step):
                    fast[k] = (i, length)
            else:
                slow[(length, rev)] = i
        self.fast = fast
        self.slow = slow

    def _build_vq(self, minimum, delta, sequence_p, mult):
        dims = self.dims
        vecs = []
        lv = lookup1_values(self.entries, dims)
        for e in range(self.entries):
            last = 0.0
            div = 1
            v = []
            for _ in range(dims):
                off = (e // div) % lv
                x = mult[off] * delta + minimum + last
                if sequence_p:
                    last = x
                v.append(x)
                div *= lv
            vecs.append(tuple(v))
        self.vectors = vecs

    def decode(self, br):
        if self.single is not None:
            br.read(self.single_len)
            return self.single
        v = br.peek(FAST_BITS)
        hit = self.fast[v]
        if hit is not None:
            if br.pos + hit[1] > br.nbits:
                raise VorbisError('codeword past the end of a packet')
            br.pos += hit[1]
            return hit[0]
        for length in range(FAST_BITS + 1, self.maxlen + 1):
            k = br.peek(length)
            e = self.slow.get((length, k))
            if e is not None:
                if br.pos + length > br.nbits:
                    raise VorbisError('codeword past the end of a packet')
                br.pos += length
                return e
        raise VorbisError('invalid codeword')


def parse_packed_codebook(br):
    """A codebook in Wwise's packed form (ww2ogg's 'codebook library' layout): 4-bit dims, 14-bit entries, ordered
    flag, lengths (stored as length-1), a 1-bit lookup type and the lattice values."""
    dims = br.read(4)
    entries = br.read(14)
    if dims == 0 or entries == 0:
        raise VorbisError('empty packed codebook')
    lengths = [0] * entries
    if br.read(1):  # ordered
        cur_len = br.read(5) + 1
        cur = 0
        while cur < entries:
            num = br.read(ilog(entries - cur))
            if cur + num > entries:
                raise VorbisError('ordered codebook overflows')
            for k in range(cur, cur + num):
                lengths[k] = cur_len
            cur += num
            cur_len += 1
    else:
        cll = br.read(3)
        sparse = br.read(1)
        if cll == 0 or cll > 5:
            raise VorbisError('bad codeword length field size')
        for i in range(entries):
            if not sparse or br.read(1):
                lengths[i] = br.read(cll) + 1
    lookup = br.read(1)
    if lookup == 0:
        return Codebook(dims, entries, lengths)
    minimum = float32_unpack(br.read(32))
    delta = float32_unpack(br.read(32))
    value_bits = br.read(4) + 1
    seq = br.read(1)
    n = lookup1_values(entries, dims)
    mult = [br.read(value_bits) for _ in range(n)]
    return Codebook(dims, entries, lengths, 1, minimum, delta, value_bits, bool(seq), mult)


# ------------------------------------------------------------------ WEM header

class WemInfo:
    def __init__(self):
        self.channels = 0
        self.rate = 0
        self.sample_count = 0
        self.mod_packets = True
        self.setup_offset = 0
        self.audio_offset = 0
        self.blocksize0_pow = 0
        self.blocksize1_pow = 0
        self.data_start = 0
        self.data_size = 0
        self.codec = 0


def parse_wem(data):
    """Read the RIFF chunks of a Wwise Vorbis WEM. Only the layout this game uses (fmt 0x42 with the vorb header
    inside, as written by Wwise 2012 and later) is accepted; anything else is refused with a reason."""
    if len(data) < 12 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise VorbisError('not a RIFF/WAVE file')
    pos = 12
    fmt = None
    w = WemInfo()
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack_from('<I', data, pos + 4)[0]
        body = pos + 8
        if cid == b'fmt ':
            fmt = (body, size)
        elif cid == b'data':
            w.data_start = body
            w.data_size = min(size, len(data) - body)
        pos = body + size + (size & 1 if cid != b'data' else 0)
    if fmt is None or not w.data_start:
        raise VorbisError('missing fmt or data chunk')
    fo, fs = fmt
    w.codec, w.channels, w.rate = struct.unpack_from('<HHI', data, fo)
    if w.codec != 0xFFFF:
        raise VorbisError('codec 0x%04X is not Wwise Vorbis' % w.codec)
    if fs != 0x42:
        raise VorbisError('fmt chunk of %d bytes (only the 0x42 layout is supported)' % fs)
    vo = fo + 0x18
    w.sample_count = struct.unpack_from('<I', data, vo)[0]
    mod_signal = struct.unpack_from('<I', data, vo + 4)[0]
    w.mod_packets = mod_signal not in (0x4A, 0x4B, 0x69, 0x70)
    w.setup_offset, w.audio_offset = struct.unpack_from('<II', data, vo + 0x10)
    w.blocksize0_pow = data[vo + 0x28]
    w.blocksize1_pow = data[vo + 0x29]
    if not (1 <= w.channels <= 8) or not (6 <= w.blocksize0_pow <= w.blocksize1_pow <= 13):
        raise VorbisError('implausible header (channels %d, block sizes 2^%d / 2^%d)'
                          % (w.channels, w.blocksize0_pow, w.blocksize1_pow))
    return w


def iter_packets(data, w, start):
    """(offset, payload) of each modern Wwise packet (u16 size, no granule) from data offset `start` to the end."""
    pos = w.data_start + start
    end = w.data_start + w.data_size
    while pos + 2 <= end:
        size = struct.unpack_from('<H', data, pos)[0]
        p = pos + 2
        if size == 0 or p + size > end:
            break
        yield pos, data[p:p + size]
        pos = p + size


# ------------------------------------------------------------------ setup

class Floor1:
    pass


class Residue:
    pass


class Mapping:
    pass


class Setup:
    pass


def parse_setup(packet, channels, library):
    """The Wwise-stripped setup packet: codebook ids into `library` (a list of Codebook), then floors, residues,
    mappings and modes without the fields Wwise leaves implicit."""
    br = BitReader(packet)
    s = Setup()
    n_books = br.read(8) + 1
    books = []
    for _ in range(n_books):
        cid = br.read(10)
        if cid >= len(library):
            raise VorbisError('codebook id %d is outside the library (%d books)' % (cid, len(library)))
        books.append(library[cid])
    s.books = books
    s.floors = []
    for _ in range(br.read(6) + 1):
        f = Floor1()
        parts = br.read(5)
        f.partition_class = [br.read(4) for _ in range(parts)]
        max_class = max(f.partition_class) if parts else -1
        f.class_dims, f.class_subs, f.class_master, f.sub_books = [], [], [], []
        for _c in range(max_class + 1):
            f.class_dims.append(br.read(3) + 1)
            sub = br.read(2)
            f.class_subs.append(sub)
            master = br.read(8) if sub else -1
            if master >= n_books:
                raise VorbisError('floor master book out of range')
            f.class_master.append(master)
            sb = []
            for _k in range(1 << sub):
                b = br.read(8) - 1
                if b >= n_books:
                    raise VorbisError('floor sub book out of range')
                sb.append(b)
            f.sub_books.append(sb)
        f.multiplier = br.read(2) + 1
        rangebits = br.read(4)
        xs = [0, 1 << rangebits]
        for c in f.partition_class:
            for _k in range(f.class_dims[c]):
                xs.append(br.read(rangebits))
        if len(set(xs)) != len(xs):
            raise VorbisError('floor X list has duplicates')
        f.xs = xs
        # neighbours and the sort order, precomputed
        f.low, f.high = [0, 0], [0, 0]
        for i in range(2, len(xs)):
            lo = hi = None
            for j in range(i):
                if xs[j] < xs[i] and (lo is None or xs[j] > xs[lo]):
                    lo = j
                if xs[j] > xs[i] and (hi is None or xs[j] < xs[hi]):
                    hi = j
            f.low.append(lo)
            f.high.append(hi)
        f.order = sorted(range(len(xs)), key=lambda i: xs[i])
        f.range = (256, 128, 86, 64)[f.multiplier - 1]
        f.ybits = ilog(f.range - 1)
        s.floors.append(f)
    s.residues = []
    for _ in range(br.read(6) + 1):
        r = Residue()
        r.type = br.read(2)
        if r.type > 2:
            raise VorbisError('residue type %d' % r.type)
        r.begin = br.read(24)
        r.end = br.read(24)
        r.psize = br.read(24) + 1
        r.classifications = br.read(6) + 1
        r.classbook = br.read(8)
        if r.classbook >= n_books:
            raise VorbisError('residue class book out of range')
        cascade = []
        for _c in range(r.classifications):
            low = br.read(3)
            high = br.read(5) if br.read(1) else 0
            cascade.append(high * 8 + low)
        r.books = []
        for c in range(r.classifications):
            row = []
            for k in range(8):
                if cascade[c] & (1 << k):
                    b = br.read(8)
                    if b >= n_books:
                        raise VorbisError('residue book out of range')
                    row.append(b)
                else:
                    row.append(-1)
            r.books.append(row)
        s.residues.append(r)
    s.mappings = []
    for _ in range(br.read(6) + 1):
        m = Mapping()
        m.submaps = br.read(4) + 1 if br.read(1) else 1
        m.coupling = []
        if br.read(1):
            bits = ilog(channels - 1)
            for _k in range(br.read(8) + 1):
                mag, ang = br.read(bits), br.read(bits)
                if mag == ang or mag >= channels or ang >= channels:
                    raise VorbisError('bad channel coupling')
                m.coupling.append((mag, ang))
        if br.read(2):
            raise VorbisError('mapping reserved bits are set')
        m.mux = [br.read(4) for _ in range(channels)] if m.submaps > 1 else [0] * channels
        m.submap_floor, m.submap_residue = [], []
        for _k in range(m.submaps):
            br.read(8)
            fl, rs = br.read(8), br.read(8)
            if fl >= len(s.floors) or rs >= len(s.residues):
                raise VorbisError('mapping refers to a missing floor or residue')
            m.submap_floor.append(fl)
            m.submap_residue.append(rs)
        s.mappings.append(m)
    s.modes = []
    for _ in range(br.read(6) + 1):
        flag = br.read(1)
        mp = br.read(8)
        if mp >= len(s.mappings):
            raise VorbisError('mode refers to a missing mapping')
        s.modes.append((flag, mp))
    s.mode_bits = ilog(len(s.modes) - 1)
    return s


# ------------------------------------------------------------------ transforms

_FFT_CACHE = {}


def _fft_plan(n):
    plan = _FFT_CACHE.get(n)
    if plan is None:
        bits = n.bit_length() - 1
        rev = [int(format(i, '0%db' % bits)[::-1], 2) if bits else 0 for i in range(n)]
        tw = [cmath.exp(-2j * math.pi * k / n) for k in range(n // 2)]
        plan = (rev, tw)
        _FFT_CACHE[n] = plan
    return plan


def fft(a):
    """In-order iterative radix-2 forward FFT (len(a) a power of two)."""
    n = len(a)
    rev, tw = _fft_plan(n)
    a = [a[r] for r in rev]
    size = 2
    while size <= n:
        half = size // 2
        step = n // size
        for start in range(0, n, size):
            k = 0
            for j in range(start, start + half):
                t = a[j + half] * tw[k]
                u = a[j]
                a[j] = u + t
                a[j + half] = u - t
                k += step
        size *= 2
    return a


_DCT4_CACHE = {}


def dct4(x):
    """DCT-IV: u[m] = sum_k x[k] cos(pi/M (m + 1/2)(k + 1/2)), M = len(x) (a power of two >= 2), via an M/2 FFT."""
    m = len(x)
    h = m // 2
    tw = _DCT4_CACHE.get(m)
    if tw is None:
        pre = [cmath.exp(-1j * math.pi * (j + 0.25) / m) for j in range(h)]
        post = [cmath.exp(-1j * math.pi * k / m) for k in range(h)]
        tw = (pre, post)
        _DCT4_CACHE[m] = tw
    pre, post = tw
    t = [complex(x[2 * j], x[m - 1 - 2 * j]) * pre[j] for j in range(h)]
    T = fft(t)
    u = [0.0] * m
    for k in range(h):
        c = T[k] * post[k]
        u[2 * k] = c.real
        u[m - 1 - 2 * k] = -c.imag
    return u


def imdct(X):
    """Vorbis IMDCT (spec section 1.3.2, unnormalised): y[n] = sum_k X[k] cos(2pi/N (n + 1/2 + N/4)(k + 1/2)),
    N = 2 len(X); computed as a DCT-IV of size N/2 plus its symmetries."""
    m = len(X)
    u = dct4(X)
    n = 2 * m
    q = m // 2
    y = [0.0] * n
    for i in range(n):
        j = i + q
        if j < m:
            y[i] = u[j]
        elif j < 2 * m:
            y[i] = -u[2 * m - 1 - j]
        else:
            y[i] = -u[j - 2 * m]
    return y


def imdct_naive(X):
    m = len(X)
    n = 2 * m
    return [sum(X[k] * math.cos(2 * math.pi / n * (i + 0.5 + n / 4) * (k + 0.5)) for k in range(m))
            for i in range(n)]


_WIN_CACHE = {}


def window_slope(n):
    """The Vorbis power-sine window's rising half for a block of size n (n/2 values)."""
    w = _WIN_CACHE.get(n)
    if w is None:
        h = n // 2
        w = [math.sin(0.5 * math.pi * math.sin((i + 0.5) / h * 0.5 * math.pi) ** 2) for i in range(h)]
        _WIN_CACHE[n] = w
    return w


def block_window(n, prev_n, next_n):
    """The full window of a block of size n whose neighbours have sizes prev_n and next_n."""
    win = [0.0] * n
    lw = min(n, prev_n)
    rw = min(n, next_n)
    ls = window_slope(lw)
    rs = window_slope(rw)
    lstart = n // 4 - lw // 4
    for i in range(lw // 2):
        win[lstart + i] = ls[i]
    for i in range(lstart + lw // 2, n // 2):
        win[i] = 1.0
    rstart = 3 * n // 4 - rw // 4
    for i in range(n // 2, rstart):
        win[i] = 1.0
    for i in range(rw // 2):
        win[rstart + i] = rs[rw // 2 - 1 - i]
    return win


# floor1 inverse dB table: a geometric series from 1.0649863e-07 to 1.0 (spec section 10.1)
INVERSE_DB = [1.0649863e-07 * (1.0 / 1.0649863e-07) ** (i / 255.0) for i in range(256)]


def _render_point(x0, y0, x1, y1, x):
    dy = y1 - y0
    adx = x1 - x0
    off = abs(dy) * (x - x0) // adx
    return y0 - off if dy < 0 else y0 + off


def _render_line(x0, y0, x1, y1, v, n):
    dy = y1 - y0
    adx = x1 - x0
    ady = abs(dy)
    base = int(dy / adx)  # truncated toward zero, as in C
    sy = base - 1 if dy < 0 else base + 1
    ady -= abs(base) * adx
    x, y, err = x0, y0, 0
    if x < n:
        v[x] = y
    for x in range(x0 + 1, min(x1, n)):
        err += ady
        if err >= adx:
            err -= adx
            y += sy
        else:
            y += base
        v[x] = y


def floor1_decode(f, books, br):
    """The floor's Y values, or None when the floor is unused in this packet."""
    if not br.read(1):
        return None
    ys = [br.read(f.ybits), br.read(f.ybits)]
    for c in f.partition_class:
        cdim = f.class_dims[c]
        cbits = f.class_subs[c]
        csub = (1 << cbits) - 1
        cval = books[f.class_master[c]].decode(br) if cbits else 0
        sb = f.sub_books[c]
        for _ in range(cdim):
            b = sb[cval & csub]
            cval >>= cbits
            ys.append(books[b].decode(br) if b >= 0 else 0)
    return ys


def floor1_curve(f, ys, n):
    """Floor curve values (linear gains) for n = blocksize / 2 bins."""
    xs = f.xs
    cnt = len(xs)
    rng = f.range
    final = [0] * cnt
    step2 = [False] * cnt
    final[0], final[1] = ys[0], ys[1]
    step2[0] = step2[1] = True
    for i in range(2, cnt):
        lo, hi = f.low[i], f.high[i]
        pred = _render_point(xs[lo], final[lo], xs[hi], final[hi], xs[i])
        val = ys[i]
        highroom = rng - pred
        lowroom = pred
        room = highroom * 2 if highroom < lowroom else lowroom * 2
        if val:
            step2[lo] = step2[hi] = step2[i] = True
            if val >= room:
                final[i] = val - lowroom + pred if highroom > lowroom else pred - val + highroom - 1
            elif val & 1:
                final[i] = pred - (val + 1) // 2
            else:
                final[i] = pred + val // 2
        else:
            final[i] = pred
    mult = f.multiplier
    v = [0] * n
    order = f.order
    lx = 0
    ly = final[order[0]] * mult
    hx = 0
    hy = ly
    for k in order[1:]:
        if step2[k]:
            hy = final[k] * mult
            hx = xs[k]
            _render_line(lx, ly, hx, hy, v, n)
            lx, ly = hx, hy
    if hx < n:
        _render_line(hx, hy, n, hy, v, n)
    tab = INVERSE_DB
    return [tab[y] if 0 <= y < 256 else (0.0 if y < 0 else 1.0) for y in v]


def residue_decode(r, books, br, vectors, do_decode, n):
    """Adds the residue of one submap into `vectors` (one list of n = blocksize/2 floats per channel)."""
    ch = len(vectors)
    if r.type == 2:
        if not any(do_decode):
            return
        flat = [0.0] * (n * ch)
        _residue_partitions(r, books, br, [flat], [True], n * ch, 1)
        for i in range(n):
            for c in range(ch):
                vectors[c][i] += flat[i * ch + c]
        return
    _residue_partitions(r, books, br, vectors, do_decode, n, r.type)


def _residue_partitions(r, books, br, vectors, do_decode, actual, rtype):
    lb = min(r.begin, actual)
    le = min(r.end, actual)
    psize = r.psize
    to_read = (le - lb) // psize
    if to_read <= 0:
        return
    cbook = books[r.classbook]
    cpw = cbook.dims
    ncls = r.classifications
    chans = [c for c in range(len(vectors)) if do_decode[c]]
    classes = {c: [0] * (to_read + cpw) for c in chans}
    for pas in range(8):
        pc = 0
        while pc < to_read:
            if pas == 0:
                for c in chans:
                    temp = cbook.decode(br)
                    cl = classes[c]
                    for i in range(cpw - 1, -1, -1):
                        cl[i + pc] = temp % ncls
                        temp //= ncls
            i = 0
            while i < cpw and pc < to_read:
                for c in chans:
                    b = r.books[classes[c][pc]][pas]
                    if b < 0:
                        continue
                    book = books[b]
                    if book.vectors is None:
                        raise VorbisError('residue book without values')
                    v = vectors[c]
                    off = lb + pc * psize
                    dim = book.dims
                    vecs = book.vectors
                    if rtype == 0:
                        step = psize // dim
                        for j in range(step):
                            vec = vecs[book.decode(br)]
                            for k in range(dim):
                                v[off + j + k * step] += vec[k]
                    else:
                        j = 0
                        while j < psize:
                            vec = vecs[book.decode(br)]
                            for k in range(dim):
                                v[off + j] += vec[k]
                                j += 1
                i += 1
                pc += 1


# ------------------------------------------------------------------ decode

def decode_wem(data, library, max_seconds=None, want=None, progress=None, mix=None):
    """Decode a Wwise Vorbis WEM to float PCM: (WemInfo, [channel][sample]), each channel an array('f'). `want`: the
    channel indexes to synthesise (the others are still read from the bitstream, as they must be, but skip the floor
    curve, transform and overlap; they come back empty). `mix`: [(channel, weight)] gives one mixed channel instead,
    mixed in the spectrum (the transform is linear), so a 5.1 track costs one transform per block and its memory is
    one channel's. `progress(fraction)` is called every few hundred packets."""
    w = parse_wem(data)
    setup_pkts = list(iter_packets(data, w, w.setup_offset))
    if not setup_pkts:
        raise VorbisError('no setup packet')
    s = parse_setup(setup_pkts[0][1], w.channels, library)
    packets = [p for _, p in iter_packets(data, w, w.audio_offset)]
    bs = (1 << w.blocksize0_pow, 1 << w.blocksize1_pow)
    ch = w.channels
    if mix is not None:
        mix = [(c, float(g)) for c, g in mix if 0 <= c < ch]
        want = {c for c, _ in mix}
    else:
        want = set(range(ch)) if want is None else {c for c in want if 0 <= c < ch}
    nout = 1 if mix is not None else ch
    # the mode (and so the block size) of every packet first: the window of a long block depends on both neighbours
    modes = []
    for p in packets:
        br = BitReader(p)
        if not w.mod_packets:
            if br.read(1):
                raise VorbisError('not an audio packet')
        mode = br.read(s.mode_bits)
        if mode >= len(s.modes):
            raise VorbisError('mode %d out of range' % mode)
        modes.append(mode)
    limit = int(max_seconds * w.rate) + bs[1] if max_seconds else None
    out = [array.array('f') for _ in range(nout)]
    tail = None          # the previous block's windowed right half (overlap), per output channel
    prev_n = None
    for idx, p in enumerate(packets):
        if progress and idx % 400 == 0:
            progress(idx / max(1, len(packets)))
        mode = modes[idx]
        blockflag, mp = s.modes[mode]
        n = bs[blockflag]
        if w.mod_packets:
            pn = bs[s.modes[modes[idx - 1]][0]] if idx > 0 else bs[0]
            nn = bs[s.modes[modes[idx + 1]][0]] if idx + 1 < len(packets) else bs[0]
            br = BitReader(p)
            br.read(s.mode_bits)
        else:
            br = BitReader(p)
            br.read(1)
            br.read(s.mode_bits)
            if blockflag:
                pn = bs[br.read(1)]
                nn = bs[br.read(1)]
            else:
                pn = nn = n
        if not blockflag:
            pn = nn = n
        half = n // 2
        m = s.mappings[mp]
        try:
            floors = []
            for c in range(ch):
                f = s.floors[m.submap_floor[m.mux[c]]]
                floors.append((f, floor1_decode(f, s.books, br)))
            nonzero = [fy[1] is not None for fy in floors]
            for mag, ang in m.coupling:
                if nonzero[mag] or nonzero[ang]:
                    nonzero[mag] = nonzero[ang] = True
            vecs = [[0.0] * half for _ in range(ch)]
            for sm in range(m.submaps):
                idxs = [c for c in range(ch) if m.mux[c] == sm]
                sub = [vecs[c] for c in idxs]
                dd = [nonzero[c] for c in idxs]
                residue_decode(s.residues[m.submap_residue[sm]], s.books, br, sub, dd, half)
        except VorbisError:
            # a packet that ends early (allowed by the spec): what was read stands, the rest is zero
            if 'floors' not in locals() or len(floors) < ch:
                floors = [(None, None)] * ch
                vecs = [[0.0] * half for _ in range(ch)]
        for mag, ang in reversed(m.coupling):
            M, A = vecs[mag], vecs[ang]
            for i in range(half):
                mv, av = M[i], A[i]
                if mv > 0:
                    if av > 0:
                        M[i], A[i] = mv, mv - av
                    else:
                        A[i], M[i] = mv, mv + av
                else:
                    if av > 0:
                        M[i], A[i] = mv, mv + av
                    else:
                        A[i], M[i] = mv, mv - av
        win = block_window(n, pn, nn)
        blocks = []
        if mix is not None:
            spec = None
            for c, g in mix:
                f, ys = floors[c]
                if ys is None:
                    continue
                curve = floor1_curve(f, ys, half)
                v = vecs[c]
                if spec is None:
                    spec = [g * v[i] * curve[i] for i in range(half)]
                else:
                    for i in range(half):
                        spec[i] += g * v[i] * curve[i]
            if spec is None:
                blocks.append([0.0] * n)
            else:
                y = imdct(spec)
                blocks.append([y[i] * win[i] for i in range(n)])
            chans = [0]
        else:
            for c in range(ch):
                f, ys = floors[c]
                if c not in want or ys is None:
                    blocks.append([0.0] * n)
                    continue
                curve = floor1_curve(f, ys, half)
                v = vecs[c]
                spec = [v[i] * curve[i] for i in range(half)]
                y = imdct(spec)
                blocks.append([y[i] * win[i] for i in range(n)])
            chans = [c for c in range(ch) if c in want]
        # overlap-add: this block's centre follows the previous one's by prev_n/4 + n/4
        for c in chans:
            y = blocks[c]
            if tail is not None:
                t = tail[c]
                tl = len(t)
                # previous right half covers [pc, pc + prev_n/2); this left half starts at its centre - n/4 offset
                start = prev_n // 4 - n // 4   # where this block's start falls inside the previous right half
                o = out[c]
                seg = [0.0] * (prev_n // 4 + n // 4)
                for i in range(len(seg)):
                    a = t[i] if i < tl else 0.0
                    j = i - start
                    b = y[j] if 0 <= j < half else 0.0
                    seg[i] = a + b
                o.extend(seg)
        # keep this block's right half, positioned from its centre
        tail = [b[half:] for b in blocks]
        prev_n = n
        if limit and max(len(o) for o in out) >= limit:
            break
    total = w.sample_count
    if total:
        out = [o[:total] for o in out]
    if progress:
        progress(1.0)
    return w, out


def downmix(w, pcm):
    """One voice channel: mono as is; stereo averaged; 5.1 and up (Wwise order FL FR FC LFE BL BR ...) the centre
    plus half of the fronts, where cutscene dialogue sits (the rears hold room tone, the LFE nothing)."""
    if w.channels == 1:
        return pcm[0]
    if w.channels == 2:
        a, b = pcm
        return [(x + y) * 0.5 for x, y in zip(a, b)]
    left, right, centre = pcm[0], pcm[1], pcm[2]
    return [c + 0.5 * (x + y) for x, y, c in zip(left, right, centre)]


def voice_channels(w):
    """The channels downmix() uses."""
    return [0] if w.channels == 1 else ([0, 1] if w.channels == 2 else [0, 1, 2])


def voice_mix(w):
    """downmix() as decode_wem(mix=...) weights."""
    if w.channels == 1:
        return [(0, 1.0)]
    if w.channels == 2:
        return [(0, 0.5), (1, 0.5)]
    return [(2, 1.0), (0, 0.5), (1, 0.5)]
