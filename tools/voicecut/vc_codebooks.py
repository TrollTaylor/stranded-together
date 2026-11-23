"""voicecut: Wwise's packed Vorbis codebook library, read from the user's own ds.exe. Python 3 stdlib only.

Wwise Vorbis setup packets name their codebooks by id into a fixed library (aoTuV 6.03, 598 books). Wwise is
statically linked into ds.exe, so the library is in the game's own image: a table of 599 pointers (preferred-base
virtual addresses, increasing: the start of each book and, last, the end of the data) into the packed book data. This module
finds that table by its shape, maps the pointers to file offsets through the PE section table and parses every book;
the books must all parse inside their own spans or the library is refused. ds.exe is opened read-only.
"""
import array
import struct

from vc_vorbis import BitReader, VorbisError, parse_packed_codebook

LIBRARY_POINTERS = 599   # 598 books and the end of the last one


class PeImage:
    def __init__(self, data):
        if data[:2] != b'MZ':
            raise VorbisError('ds.exe is not a PE image')
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise VorbisError('ds.exe has no PE header')
        nsec = struct.unpack_from('<H', data, pe + 6)[0]
        opt_size = struct.unpack_from('<H', data, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from('<H', data, opt)[0]
        if magic != 0x20B:
            raise VorbisError('ds.exe is not a 64-bit image')
        self.base = struct.unpack_from('<Q', data, opt + 24)[0]
        self.sections = []
        so = opt + opt_size
        for i in range(nsec):
            o = so + 40 * i
            vsize, va, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
            self.sections.append((va, max(vsize, rsize), raw, rsize))

    def file_offset(self, address):
        rva = address - self.base
        for va, size, raw, rsize in self.sections:
            if va <= rva < va + size:
                off = rva - va
                if off >= rsize:
                    return None
                return raw + off
        return None


def find_pointer_tables(data, base, count=LIBRARY_POINTERS, max_step=4096):
    """File offsets of u64 tables of exactly `count` increasing image addresses whose steps are 1..max_step."""
    q = array.array('Q')
    n8 = len(data) // 8 * 8
    q.frombytes(data[:n8])
    lo, hi = base, base + 0x10000000
    out = []
    n = len(q)
    i = 0
    while i < n - 1:
        a = q[i]
        if lo <= a < hi and 1 <= q[i + 1] - a <= max_step:
            j = i + 1
            while j < n and 1 <= q[j] - q[j - 1] <= max_step:
                j += 1
            if j - i == count:
                out.append(i * 8)
            i = j
        else:
            i += 1
    return out


def load_library(data):
    """Parse the codebook library out of ds.exe's bytes: a list of 598 Codebook objects."""
    pe = PeImage(data)
    errors = []
    for table in find_pointer_tables(data, pe.base):
        ptrs = struct.unpack_from('<%dQ' % LIBRARY_POINTERS, data, table)
        try:
            books = []
            for k, p in enumerate(ptrs[:-1]):
                off = pe.file_offset(p)
                if off is None:
                    raise VorbisError('book %d is outside the file' % k)
                span = ptrs[k + 1] - p
                chunk = data[off:off + span]
                br = BitReader(chunk)
                books.append(parse_packed_codebook(br))
                if not 0 <= span - (br.pos + 7) // 8 <= 1:   # padded to a byte, sometimes one more
                    raise VorbisError('book %d does not end its span (%d of %d bytes)' % (k, (br.pos + 7) // 8, span))
            return books
        except (VorbisError, struct.error, IndexError) as e:
            errors.append('table at 0x%X: %s' % (table, e))
    raise VorbisError('the Wwise codebook library was not found in ds.exe'
                      + (' (%s)' % '; '.join(errors) if errors else ''))
