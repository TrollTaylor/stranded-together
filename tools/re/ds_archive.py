"""Read-only reader for Death Stranding Director's Cut (Decima) packfiles.

The game keeps its data in data/*.bin "packfiles". This tool lists them, resolves core
paths by hashing candidate path lists, extracts single core files and lists the
RTTI objects inside a core file. It never writes into the game folder: everything goes
to the output directory (--out, default: the session scratchpad or %TEMP%).

Format (learned from the open-source readers ProjectDecima_python (REDxEYE) and
decima-workshop (ShadelessFox), and confirmed on the installed game by this tool):

  Header (40 bytes)
    u32 magic        0x20304050 plain, 0x21304050 encrypted (all DS DC archives)
    u32 key
    then 32 bytes, XOR-encrypted when magic is 0x21304050:
    u64 file_size, u64 data_size, u64 entry_count, u32 chunk_count, u32 max_chunk_size
  Entry table: entry_count x 32 bytes
    u32 index, u32 key0, u64 path_hash, u64 offset, u32 size, u32 key1
    (offset/size are in the uncompressed stream)
  Chunk table: chunk_count x 32 bytes
    u64 uncompressed_offset, u32 uncompressed_size, u32 key0,
    u64 compressed_offset, u32 compressed_size, u32 key1
  Data: Oodle-compressed chunks, each XOR-encrypted.

  Table decryption: two 16-byte masks = MurmurHash3_x64_128(seed 42) of
    {key0, SALT1[1], SALT1[2], SALT1[3]} and {key1, SALT1[1..3]} (the header uses key and
    key+1). The key words themselves are kept as read (for chunks key0 is restored).
  Chunk data decryption: iv = MurmurHash3_x64_128(first 16 bytes of the decrypted chunk
    entry) XOR SALT2, mask = MD5(iv), data ^= mask repeated.
  Path hash: first u64 of MurmurHash3_x64_128(seed 42) of the lower-case UTF-8 path
    including its extension (".core", ".core.stream") plus a terminating NUL.

  Core file: a sequence of objects, each u64 type_id, u32 size, then `size` bytes of
    fields in serialized order (see _ordered_fields). ObjectUUID is usually the first
    field, but not for EntityResource types (two bools come first): object_guid() reads
    the fields before it. Stream files are "<path>.core.stream".
  type_id is the low u64 of MurmurHash3_x64_128 over a type signature string built
  from the full RTTI definition (port of decima-workshop's RTTITypeDumper). The type
  definitions come from an RTTI type JSON given with --types (decima-workshop's
  dsdc_types.json, a data file; not shipped here). `typecheck` compares that JSON with
  the ds.exe RTTI catalog of tools/re/rtti.py; `core --decode` reads object fields.

Oodle: the game's own oo2core_7_win64.dll is loaded read-only from the game folder.

Usage (python 3.10+, stdlib + ctypes only):
  python tools/re/ds_archive.py archives
  python tools/re/ds_archive.py index                      # build entry index cache
  python tools/re/ds_archive.py entries <archive> [--limit N]
  python tools/re/ds_archive.py resolve <paths.txt>...     # one path per line
  python tools/re/ds_archive.py hash <path>...
  python tools/re/ds_archive.py extract <core path or 0xHASH>   # to <out>/extracted/
  python tools/re/ds_archive.py core <core path or 0xHASH> [--types T.json.gz]
         [--strings] [--decode TYPE_REGEX] [--json]
  python tools/re/ds_archive.py scan <path regex> --types T.json.gz   # JSON lines
  python tools/re/ds_archive.py typecheck --types T.json.gz
  python tools/re/ds_archive.py prefetch                   # dump the prefetch path list
  python tools/re/ds_archive.py grep <regex> [--lists F]   # search known paths
Common options: --game DIR, --out DIR.
"""
from __future__ import annotations

import argparse
import ctypes
import gzip
import hashlib
import json
import os
import re
import struct
import sys
import uuid

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT"
SEED = 42
SALT1 = (0x0FA3A9443, 0x0F41CAB62, 0x0F376811C, 0x0D2A89E3E)
SALT2 = (0x06C084A37, 0x07E159D95, 0x03D5AF7E8, 0x018AA7D3F)
MAGIC_PLAIN = 0x20304050
MAGIC_ENC = 0x21304050
PREFETCH_PATH = "prefetch/fullgame.prefetch.core"
M64 = (1 << 64) - 1


# ---------------------------------------------------------------- hashing

def _rotl(x: int, r: int) -> int:
    return ((x << r) | (x >> (64 - r))) & M64


def _fmix(k: int) -> int:
    k ^= k >> 33
    k = (k * 0xFF51AFD7ED558CCD) & M64
    k ^= k >> 33
    k = (k * 0xC4CEB9FE1A85EC53) & M64
    k ^= k >> 33
    return k


def murmur3_x64_128(data: bytes, seed: int = SEED) -> tuple[int, int]:
    c1, c2 = 0x87C37B91114253D5, 0x4CF5AD432745937F
    h1 = h2 = seed & M64
    n = len(data)
    nblocks = n // 16
    for i in range(nblocks):
        k1, k2 = struct.unpack_from("<QQ", data, i * 16)
        k1 = (k1 * c1) & M64
        k1 = _rotl(k1, 31)
        k1 = (k1 * c2) & M64
        h1 ^= k1
        h1 = _rotl(h1, 27)
        h1 = (h1 + h2) & M64
        h1 = (h1 * 5 + 0x52DCE729) & M64
        k2 = (k2 * c2) & M64
        k2 = _rotl(k2, 33)
        k2 = (k2 * c1) & M64
        h2 ^= k2
        h2 = _rotl(h2, 31)
        h2 = (h2 + h1) & M64
        h2 = (h2 * 5 + 0x38495AB5) & M64
    tail = data[nblocks * 16:]
    k1 = k2 = 0
    t = len(tail)
    if t > 8:
        k2 = int.from_bytes(tail[8:], "little")
        k2 = (k2 * c2) & M64
        k2 = _rotl(k2, 33)
        k2 = (k2 * c1) & M64
        h2 ^= k2
    if t > 0:
        k1 = int.from_bytes(tail[:8], "little")
        k1 = (k1 * c1) & M64
        k1 = _rotl(k1, 31)
        k1 = (k1 * c2) & M64
        h1 ^= k1
    h1 ^= n
    h2 ^= n
    h1 = (h1 + h2) & M64
    h2 = (h2 + h1) & M64
    h1 = _fmix(h1)
    h2 = _fmix(h2)
    h1 = (h1 + h2) & M64
    h2 = (h2 + h1) & M64
    return h1, h2


def normalize_path(path: str) -> str:
    p = path.strip().replace("\\", "/").lower()
    while p.startswith("/"):
        p = p[1:]
    if not (p.endswith(".core") or p.endswith(".stream")):
        p += ".core"
    return p


def path_hash(path: str) -> int:
    """Entry hash of a core path. 'ds/foo' and 'ds/foo.core' give the same hash."""
    return murmur3_x64_128(normalize_path(path).encode("utf-8") + b"\0")[0]


# ---------------------------------------------------------------- decryption

def _mask(key: int) -> bytes:
    h1, h2 = murmur3_x64_128(struct.pack("<4I", key & 0xFFFFFFFF, SALT1[1], SALT1[2], SALT1[3]))
    return struct.pack("<QQ", h1, h2)


def _xor(a: bytes, b: bytes) -> bytes:
    return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(len(a), "little")


def decrypt_table_row(row: bytes, key0: int, key1: int) -> bytes:
    return _xor(row, _mask(key0) + _mask(key1))


def decrypt_chunk(data: bytes, chunk_key16: bytes) -> bytes:
    h1, h2 = murmur3_x64_128(chunk_key16)
    iv = _xor(struct.pack("<QQ", h1, h2), struct.pack("<4I", *SALT2))
    mask = hashlib.md5(iv).digest()
    n = len(data)
    full = (mask * (n // 16 + 1))[:n]
    return _xor(data, full)


# ---------------------------------------------------------------- Oodle

class Oodle:
    _fn = None

    @classmethod
    def load(cls, game_dir: str):
        if cls._fn is None:
            dll = None
            for name in sorted(os.listdir(game_dir)):
                if name.lower().startswith("oo2core_") and name.lower().endswith("_win64.dll"):
                    dll = os.path.join(game_dir, name)
            if dll is None:
                raise SystemExit(f"oo2core_*_win64.dll not found in {game_dir}")
            lib = ctypes.WinDLL(dll)
            fn = lib.OodleLZ_Decompress
            fn.restype = ctypes.c_ssize_t
            fn.argtypes = [ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_void_p, ctypes.c_ssize_t,
                           ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_ssize_t,
                           ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t, ctypes.c_int]
            cls._fn = fn
        return cls._fn

    @classmethod
    def decompress(cls, game_dir: str, src: bytes, out_size: int) -> bytes:
        fn = cls.load(game_dir)
        dst = ctypes.create_string_buffer(out_size)
        got = fn(src, len(src), dst, out_size, 1, 0, 0, None, 0, None, None, None, 0, 3)
        if got != out_size:
            raise ValueError(f"Oodle decompressed {got} bytes, expected {out_size}")
        return dst.raw


# ---------------------------------------------------------------- archives

class Entry:
    __slots__ = ("index", "hash", "offset", "size")

    def __init__(self, index, h, offset, size):
        self.index, self.hash, self.offset, self.size = index, h, offset, size


class Archive:
    def __init__(self, path: str, game_dir: str):
        self.path = path
        self.name = os.path.basename(path)
        self.game_dir = game_dir
        with open(path, "rb") as f:
            hdr = f.read(40)
            magic, key = struct.unpack_from("<II", hdr)
            if magic not in (MAGIC_PLAIN, MAGIC_ENC):
                raise ValueError(f"{path}: bad magic {magic:08X}")
            self.encrypted = magic == MAGIC_ENC
            body = hdr[8:]
            if self.encrypted:
                body = decrypt_table_row(body, key, key + 1)
            (self.file_size, self.data_size, self.entry_count, self.chunk_count,
             self.max_chunk) = struct.unpack("<QQQII", body)
            self._etab = f.read(32 * self.entry_count)
            self._ctab = f.read(32 * self.chunk_count)
        self._entries = None
        self._chunks = None

    def entries(self) -> list[Entry]:
        if self._entries is None:
            out = []
            t = self._etab
            for i in range(self.entry_count):
                row = t[i * 32:(i + 1) * 32]
                if self.encrypted:
                    k0, k1 = struct.unpack_from("<I", row, 4)[0], struct.unpack_from("<I", row, 28)[0]
                    row = decrypt_table_row(row, k0, k1)
                idx, _k0, h, off, size, _k1 = struct.unpack("<IIQQII", row)
                out.append(Entry(idx, h, off, size))
            self._entries = out
        return self._entries

    def chunks(self) -> list[tuple]:
        if self._chunks is None:
            out = []
            t = self._ctab
            for i in range(self.chunk_count):
                raw = t[i * 32:(i + 1) * 32]
                row = raw
                k0 = struct.unpack_from("<I", raw, 12)[0]
                if self.encrypted:
                    k1 = struct.unpack_from("<I", raw, 28)[0]
                    row = decrypt_table_row(raw, k0, k1)
                uoff, usize, _k, coff, csize, _k1 = struct.unpack("<QIIQII", row)
                key16 = struct.pack("<QII", uoff, usize, k0)
                out.append((uoff, usize, coff, csize, key16))
            self._chunks = out
        return self._chunks

    def read(self, offset: int, size: int) -> bytes:
        chunks = self.chunks()
        # chunks are sorted by uncompressed offset; binary search the first one
        lo, hi = 0, len(chunks) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if chunks[mid][0] <= offset:
                lo = mid
            else:
                hi = mid - 1
        out = bytearray()
        end = offset + size
        i = lo
        with open(self.path, "rb") as f:
            while i < len(chunks) and chunks[i][0] < end:
                uoff, usize, coff, csize, key16 = chunks[i]
                f.seek(coff)
                data = f.read(csize)
                if self.encrypted:
                    data = decrypt_chunk(data, key16)
                if csize == usize:
                    plain = data  # stored (not observed; kept for safety)
                else:
                    plain = Oodle.decompress(self.game_dir, data, usize)
                a = max(offset, uoff) - uoff
                b = min(end, uoff + usize) - uoff
                out += plain[a:b]
                i += 1
        if len(out) != size:
            raise ValueError(f"short read {len(out)} of {size}")
        return bytes(out)


class Game:
    def __init__(self, game_dir: str, out_dir: str):
        self.game_dir = game_dir
        self.data_dir = os.path.join(game_dir, "data")
        self.out_dir = out_dir
        self._archives = {}
        self._index = None

    def archive_names(self) -> list[str]:
        return sorted(n for n in os.listdir(self.data_dir) if n.lower().endswith(".bin"))

    def archive(self, name: str) -> Archive:
        if name not in self._archives:
            self._archives[name] = Archive(os.path.join(self.data_dir, name), self.game_dir)
        return self._archives[name]

    # index: hash -> list of (archive, offset, size); cached in out_dir
    def index(self, rebuild=False) -> dict:
        if self._index is not None and not rebuild:
            return self._index
        cache = os.path.join(self.out_dir, "ds_archive_index.json.gz")
        stamp = {n: os.path.getsize(os.path.join(self.data_dir, n)) for n in self.archive_names()}
        if os.path.exists(cache) and not rebuild:
            with gzip.open(cache, "rt") as f:
                c = json.load(f)
            if c.get("stamp") == stamp:
                self._index = {int(k, 16): v for k, v in c["entries"].items()}
                return self._index
        idx: dict[int, list] = {}
        for n in self.archive_names():
            a = self.archive(n)
            for e in a.entries():
                idx.setdefault(e.hash, []).append([n, e.offset, e.size])
            print(f"  indexed {n}: {a.entry_count} entries", file=sys.stderr)
        os.makedirs(self.out_dir, exist_ok=True)
        with gzip.open(cache, "wt") as f:
            json.dump({"stamp": stamp, "entries": {f"{k:016x}": v for k, v in idx.items()}}, f)
        self._index = idx
        return idx

    def locate(self, key: str | int):
        h = key if isinstance(key, int) else (int(key, 16) if key.lower().startswith("0x") else path_hash(key))
        locs = self.index().get(h)
        return h, locs

    def read(self, key: str | int) -> bytes:
        h, locs = self.locate(key)
        if not locs:
            raise KeyError(f"{key}: not in any archive (hash {h:016x})")
        # A few hundred hashes exist in more than one archive (language packs and
        # duplicates). Which copy the game prefers is not known; this reads the first
        # in sorted archive order, and `resolve` lists every archive that has it.
        n, off, size = locs[0]
        return self.archive(n).read(off, size)


# ---------------------------------------------------------------- core files

def read_core_objects(data: bytes):
    """Yield (offset, type_id, size, guid or None, body) for each object.

    The guid here is the first 16 body bytes, which is the ObjectUUID only when no
    field precedes it in serialized order. Use object_guid() when the type is known."""
    pos = 0
    n = len(data)
    while pos + 12 <= n:
        tid, size = struct.unpack_from("<QI", data, pos)
        body = data[pos + 12: pos + 12 + size]
        guid = str(uuid.UUID(bytes_le=body[:16])) if size >= 16 else None
        yield pos, tid, size, guid, body
        pos += 12 + size
    if pos != n:
        raise ValueError(f"core parse ended at {pos} of {n}")


def read_hashed_string(buf: bytes, pos: int):
    (ln,) = struct.unpack_from("<I", buf, pos)
    pos += 4
    if ln == 0:
        return "", pos
    pos += 4  # crc32c of the string
    s = buf[pos:pos + ln].decode("utf-8", "replace")
    return s, pos + ln


def parse_prefetch(data: bytes):
    """PrefetchList: Files (Array<AssetPath>), Sizes (Array<int>), Links."""
    for _off, _tid, size, _guid, body in read_core_objects(data):
        pos = 16
        (count,) = struct.unpack_from("<I", body, pos)
        pos += 4
        files = []
        for _ in range(count):
            s, pos = read_hashed_string(body, pos)
            files.append(s)
        (scount,) = struct.unpack_from("<I", body, pos)
        pos += 4
        sizes = list(struct.unpack_from(f"<{scount}I", body, pos))
        return files, sizes
    return [], []


STR_RX = re.compile(rb"[\x20-\x7e]{4,}")


def embedded_strings(body: bytes) -> list[str]:
    """Length-prefixed strings in an object body (Decima String / AssetPath layout)."""
    out = []
    for m in STR_RX.finditer(body):
        ms, me = m.start(), m.end()
        # The text is preceded by u32 length (String) or u32 length + u32 crc32c
        # (hashed String / Ref path). Printable crc bytes can glue onto the front of
        # the run, and a printable byte of the next field onto its end.
        found = None
        for slack in (0, 1, 2):
            for s in range(ms, min(ms + 5, me)):
                for back in (8, 4):
                    if s >= back and struct.unpack_from("<I", body, s - back)[0] == me - s - slack >= 4:
                        found = (s, me - s - slack)
                        break
                if found:
                    break
            if found:
                break
        if found:
            out.append(body[found[0]:found[0] + found[1]].decode("ascii", "replace"))
    return out


def object_guid(dec, tn, tid: int, body: bytes):
    """(uuid string, raw hex, exact?) of an object; exact=False means 'first 16 bytes'."""
    if dec is not None and tid in tn.names:
        try:
            g, raw = dec.guid(tn.names[tid], body)
            return g, raw, True
        except (ValueError, struct.error, IndexError):
            pass
    raw = body[:16]
    return str(uuid.UUID(bytes_le=raw)) if len(raw) == 16 else None, raw.hex(), False


# ---------------------------------------------------------------- type names

class TypeNames:
    """type_id -> name (and base classes), from a decima-workshop style type JSON."""

    def __init__(self, json_path: str | None):
        self.names: dict[int, str] = {}
        self.decl: dict = {}
        if json_path:
            op = gzip.open if json_path.endswith(".gz") else open
            with op(json_path, "rt", encoding="utf-8") as f:
                self.decl = json.load(f)
            self.names.update(compute_type_ids(json_path))

    def name(self, tid: int) -> str:
        return self.names.get(tid, f"?{tid:016X}")

    def derives(self, name: str, base: str) -> bool:
        seen, stack = set(), [name]
        while stack:
            n = stack.pop()
            if n == base:
                return True
            if n in seen or n not in self.decl:
                continue
            seen.add(n)
            stack += [b["name"] for b in self.decl[n].get("bases", [])]
        return False


def compute_type_ids(json_path: str) -> dict[int, str]:
    """Port of decima-workshop RTTITypeDumper: type id of every RTTIRefObject class."""
    op = gzip.open if json_path.endswith(".gz") else open
    with op(json_path, "rt", encoding="utf-8") as f:
        decl = json.load(f)
    spec = decl.pop("$spec", {"version": 1})
    ver = int(spec.get("version", 1))

    def is_instance(name, base):
        if name == base:
            return True
        d = decl.get(name)
        if not d:
            return False
        return any(is_instance(b["name"], base) for b in d.get("bases", []))

    cache: dict[str, tuple[int, int]] = {}
    pending: set[str] = set()

    def parse_param(tn: str):
        # "Array<Ref<Foo>>" -> ("Array", "Ref<Foo>")
        if tn.endswith(">") and "<" in tn:
            i = tn.index("<")
            return tn[:i], tn[i + 1:-1]
        return None, None

    def full_name(tn: str) -> str:
        outer, inner = parse_param(tn)
        if outer is None:
            return tn
        return outer + "_" + full_name(inner)

    def cls_version(d):
        return int(d.get("version" if ver > 2 else ("flags1" if ver > 1 else "unknownC"), 0))

    def cls_flags(d):
        return int(d.get("flags" if ver > 2 else ("flags2" if ver > 1 else "flags"), 0))

    def find_flags(tn, mask):
        d = decl.get(tn)
        if not d:
            return 0
        fl = cls_flags(d)
        if fl & mask:
            return fl
        for b in d.get("bases", []):
            r = find_flags(b["name"], mask)
            if r:
                return r
        return 0

    def ordered_fields(tn):
        return _ordered_fields(decl, tn)

    def hexid(tid):
        lo, hi = tid
        return (lo.to_bytes(8, "little") + hi.to_bytes(8, "little")).hex()

    def short_id(tn):
        return mm("RTTIBinaryVersion: 2, Type: " + full_name(tn))

    def nested(tn):
        return short_id(tn) if tn in pending else type_id(tn)

    def base_info(tn, indent, sb):
        sb.append("  " * indent)
        d = decl.get(tn)
        if d and d.get("type") == "class":
            sb.append(f"{full_name(tn)} {cls_version(d):X} {find_flags(tn, 0xFFF):X}\n")
            for b in d.get("bases", []):
                base_info(b["name"], indent + 1, sb)
        else:
            sb.append(f"{full_name(tn)} 0 0\n")

    def type_id(tn):
        if tn in cache:
            return cache[tn]
        pending.add(tn)
        try:
            sb = [f"RTTIBinaryVersion: 2, Type: {full_name(tn)}\n"]
            d = decl.get(tn)
            kind = d.get("type") if d else None
            if kind == "class":
                for m, _off, fl in ordered_fields(tn):
                    cat = m.get("category") or "(none)"
                    sb.append(f"Attr: {hexid(nested(m['type']))} {cat} {m['name']} {fl & 0xDEB}\n")
            base_info(tn, 0, sb)
            if kind in ("enum", "enum flags"):
                if kind == "enum":
                    sb.append(f"Enum-Size: {int(d.get('size', 0))}\n")
                for m in d.get("members", []):
                    sb.append(f"Enumeration-Value: {m['value']} {m['name']}\n")
            outer, inner = parse_param(tn)
            if outer is not None and outer in CONTAINERS:
                sb.append(f"Contained-Type: {hexid(nested(inner))}\n")
            r = mm("".join(sb))
            cache[tn] = r
            return r
        finally:
            pending.discard(tn)

    def mm(s):
        return murmur3_x64_128(s.encode("utf-8"))

    out = {}
    for name, d in decl.items():
        if d.get("type") == "class" and is_instance(name, "RTTIRefObject"):
            try:
                out[type_id(name)[0]] = name
            except (KeyError, RecursionError):
                pass
    return out


def _ordered_fields(decl: dict, tn: str) -> list:
    """Serialized field order of a class: all members of the class and its bases at their
    absolute offsets, save-state members (flag 2) removed, sorted by offset with
    decima-workshop's deterministic quicksort. The same order feeds the type id."""
    fields = []

    def collect(t, off):
        d = decl[t]
        for b in d.get("bases", []):
            collect(b["name"], int(b.get("offset", 0)) + off)
        for m in d.get("members", []):
            fields.append([m, int(m.get("offset", 0)) + off, int(m.get("flags", 0))])
    collect(tn, 0)
    fields = [f for f in fields if not (f[2] & 2)]
    _quicksort(fields, lambda f: f[1])
    return fields


_PRIM = {
    "bool": "<?", "int8": "<b", "uint8": "<B", "int16": "<h", "uint16": "<H", "int": "<i",
    "int32": "<i", "uint": "<I", "uint32": "<I", "float": "<f", "int64": "<q", "uint64": "<Q",
    "double": "<d", "ucs4": "<I", "wchar": "<H", "tchar": "<B", "HalfFloat": "<e", "uintptr": "<Q",
}
REFS = {"Ref", "cptr", "StreamingRef", "UUIDRef", "WeakPtr"}


class Decoder:
    """Generic reader for RTTI-serialized object bodies, driven by the type JSON.

    Layout rules (decima-workshop's readers): primitives little-endian; String =
    u32 length, then u32 crc32c and the bytes when length > 0; WString = u32 length and
    UTF-16 units; Array/HashMap/HashSet = u32 count and the items; Ref-like = u8 kind,
    then a GUID unless kind is 0, then a path String when kind >= 2; enums by their size;
    classes field by field in _ordered_fields order. A class that handles MsgReadBinary
    keeps custom data after its fields; it is returned raw as '_extra'.
    """

    def __init__(self, tn: "TypeNames"):
        self.decl = tn.decl
        self._fields = {}

    def fields(self, cls):
        if cls not in self._fields:
            self._fields[cls] = _ordered_fields(self.decl, cls)
        return self._fields[cls]

    def has_readbinary(self, cls):
        d = self.decl.get(cls) or {}
        if "MsgReadBinary" in d.get("messages", []):
            return True
        return any(self.has_readbinary(b["name"]) for b in d.get("bases", []))

    def guid(self, cls: str, body: bytes):
        """ObjectUUID of an object: read the fields that precede it in serialized order
        (for EntityResource types two bools come first). Returns (uuid string, raw hex)."""
        pos = 0
        for m, _off, _fl in self.fields(cls):
            if m["name"] == "ObjectUUID":
                raw = body[pos:pos + 16]
                return str(uuid.UUID(bytes_le=raw)), raw.hex()
            _v, pos = self.read(m["type"], body, pos)
        raise ValueError(f"{cls} has no ObjectUUID")

    def object(self, cls: str, body: bytes):
        val, pos = self.read(cls, body, 0, top=True)
        if pos < len(body):
            val["_extra_bytes"] = len(body) - pos
        elif pos > len(body):
            raise ValueError(f"{cls}: read past end ({pos} > {len(body)})")
        return val

    def read(self, tn: str, b: bytes, pos: int, top=False):
        if tn in _PRIM:
            v = struct.unpack_from(_PRIM[tn], b, pos)[0]
            return v, pos + struct.calcsize(_PRIM[tn])
        if tn in ("String", "Filename", "CommandLine"):
            (ln,) = struct.unpack_from("<I", b, pos)
            pos += 4
            if ln == 0:
                return "", pos
            return b[pos + 4:pos + 4 + ln].decode("utf-8", "replace"), pos + 4 + ln
        if tn == "WString":
            (ln,) = struct.unpack_from("<I", b, pos)
            pos += 4
            return b[pos:pos + 2 * ln].decode("utf-16-le", "replace"), pos + 2 * ln
        if tn == "GGUUID":
            return str(uuid.UUID(bytes_le=b[pos:pos + 16])), pos + 16
        if tn.endswith(">") and "<" in tn:
            outer, inner = tn[:tn.index("<")], tn[tn.index("<") + 1:-1]
            if outer in REFS:
                kind = b[pos]
                pos += 1
                if kind == 0:
                    return None, pos
                g = str(uuid.UUID(bytes_le=b[pos:pos + 16]))
                pos += 16
                if kind >= 2:
                    path, pos = read_hashed_string(b, pos)
                    return {"kind": kind, "guid": g, "path": path}, pos
                return {"kind": kind, "guid": g}, pos
            if outer in CONTAINERS:
                (n,) = struct.unpack_from("<I", b, pos)
                pos += 4
                hashed = outer in ("HashMap", "HashSet")  # each item has a u32 checksum first
                if inner in ("uint8", "int8") and not hashed:
                    return f"<{n} bytes>", pos + n
                out = []
                for _ in range(n):
                    if hashed:
                        pos += 4
                    v, pos = self.read(inner, b, pos)
                    out.append(v)
                return out, pos
            raise ValueError(f"unknown template {outer}")
        d = self.decl.get(tn)
        if d is None:
            raise ValueError(f"unknown type {tn}")
        kind = d.get("type")
        if kind == "primitive":
            return self.read(d.get("base") or d.get("parent_type"), b, pos)
        if kind in ("enum", "enum flags"):
            size = int(d.get("size", 4))
            v = int.from_bytes(b[pos:pos + size], "little")
            if kind == "enum":
                names = [m["name"] for m in d.get("members", []) if m["value"] == v]
                v = names[0] if names else v
            return v, pos + size
        if kind == "class":
            out = {}
            for m, _off, _fl in self.fields(tn):
                out[m["name"]], pos = self.read(m["type"], b, pos)
            if not top and self.has_readbinary(tn):
                raise ValueError(f"{tn} has custom binary data inside another object")
            return out, pos
        raise ValueError(f"cannot read {tn} ({kind})")


# Template names that are containers in decima-workshop (RTTITypeArray / RTTITypeHashMap);
# Ref, cptr, StreamingRef, UUIDRef and WeakPtr are references and carry no Contained-Type.
CONTAINERS = {
    "Array", "HashMap", "HashSet", "EnvelopeSegment_MAX_ENVELOPE_SEGMENTS",
    "GlobalAppRenderVariableInfo_GLOBAL_APP_RENDER_VAR_COUNT",
    "GlobalRenderVariableInfo_GLOBAL_RENDER_VAR_COUNT", "ShaderProgramResourceSet_36",
    "float_GLOBAL_APP_RENDER_VAR_COUNT", "float_GLOBAL_RENDER_VAR_COUNT",
    "uint16_PBD_MAX_SKIN_WEIGHTS", "uint64_PLACEMENT_LAYER_MASK_SIZE", "uint8_PBD_MAX_SKIN_WEIGHTS",
}


def _quicksort(items, key):
    """decima-workshop's deterministic quicksort (same order for equal keys)."""
    def qs(left, right, state):
        if left < right:
            state = (0x19660D * state + 0x3C6EF35F) & 0xFFFFFFFF
            pivot = (state >> 8) % (right - left)
            items[left + pivot], items[right] = items[right], items[left + pivot]
            start = left - 1
            end = right
            while True:
                start += 1
                while start < end and key(items[start]) < key(items[right]):
                    start += 1
                end -= 1
                while end > start and key(items[right]) < key(items[end]):
                    end -= 1
                if start >= end:
                    break
                items[start], items[end] = items[end], items[start]
            items[start], items[right] = items[right], items[start]
            state = qs(left, start - 1, state)
            state = qs(start + 1, right, state)
        return state
    qs(0, len(items) - 1, 0)


# ---------------------------------------------------------------- CLI

def default_out() -> str:
    sp = os.environ.get("CLAUDE_SCRATCHPAD") or os.path.join(os.environ.get("TEMP", "."), "ds_archive")
    return sp


def check_out_dir(out: str, game: str):
    a = os.path.normcase(os.path.abspath(out))
    g = os.path.normcase(os.path.abspath(game))
    if a == g or a.startswith(g + os.sep):
        raise SystemExit("refusing to write inside the game folder")


def read_path_lists(files) -> list[str]:
    paths = []
    for fn in files:
        op = gzip.open if fn.endswith(".gz") else open
        with op(fn, "rt", encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if s and not s.startswith("#"):
                    paths.append(s)
    return paths


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--out", default=default_out())
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("archives")
    p = sub.add_parser("index")
    p.add_argument("--rebuild", action="store_true")
    p = sub.add_parser("entries")
    p.add_argument("archive")
    p.add_argument("--limit", type=int, default=0)
    p.add_argument("--names", nargs="*", default=[], help="path lists to name entries")
    p = sub.add_parser("hash")
    p.add_argument("paths", nargs="+")
    p = sub.add_parser("resolve")
    p.add_argument("lists", nargs="+")
    p.add_argument("--found", help="write found paths to this file (inside --out)")
    p = sub.add_parser("extract")
    p.add_argument("key")
    p = sub.add_parser("core")
    p.add_argument("key")
    p.add_argument("--types")
    p.add_argument("--strings", action="store_true")
    p.add_argument("--json", action="store_true")
    p.add_argument("--decode", default="", help="regex of type names whose fields to decode")
    sub.add_parser("prefetch")
    p = sub.add_parser("scan", help="parse every known core path matching a regex; JSON lines out")
    p.add_argument("regex")
    p.add_argument("--types")
    p.add_argument("--lists", nargs="*", default=[], help="extra path lists besides the prefetch")
    p.add_argument("--output", default="scan.jsonl", help="file name inside --out")
    p = sub.add_parser("typecheck", help="compare the type JSON with the ds.exe RTTI catalog")
    p.add_argument("--types", required=True)
    p.add_argument("--rtti", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".cache", "rtti.json"))
    p = sub.add_parser("grep")
    p.add_argument("regex")
    p.add_argument("--lists", nargs="*", default=[])
    a = ap.parse_args(argv)

    check_out_dir(a.out, a.game)
    os.makedirs(a.out, exist_ok=True)
    g = Game(a.game, a.out)

    if a.cmd == "archives":
        for n in g.archive_names():
            ar = g.archive(n)
            print(f"{n}  enc={int(ar.encrypted)} entries={ar.entry_count:6d} chunks={ar.chunk_count:6d} "
                  f"max_chunk=0x{ar.max_chunk:X} data={ar.data_size} file={ar.file_size}")
    elif a.cmd == "index":
        idx = g.index(rebuild=a.rebuild)
        dup = sum(1 for v in idx.values() if len(v) > 1)
        print(f"{len(idx)} unique hashes, {dup} present in more than one archive")
    elif a.cmd == "entries":
        names = {path_hash(p): normalize_path(p) for p in read_path_lists(a.names)}
        ar = g.archive(a.archive)
        for i, e in enumerate(ar.entries()):
            if a.limit and i >= a.limit:
                break
            print(f"{e.index:6d} {e.hash:016x} off={e.offset:12d} size={e.size:10d} {names.get(e.hash, '')}")
    elif a.cmd == "hash":
        for p in a.paths:
            print(f"{path_hash(p):016x}  {normalize_path(p)}")
    elif a.cmd == "resolve":
        idx = g.index()
        found = []
        seen = set()
        for p in read_path_lists(a.lists):
            np_ = normalize_path(p)
            if np_ in seen:
                continue
            seen.add(np_)
            h = path_hash(np_)
            if h in idx:
                found.append(np_)
                locs = idx[h]
                print(f"{h:016x} {locs[0][2]:10d} {','.join(x[0][:8] for x in locs)} {np_}")
        print(f"# {len(found)} of {len(seen)} candidate paths exist", file=sys.stderr)
        if a.found:
            fp = os.path.join(a.out, os.path.basename(a.found))
            with open(fp, "w", encoding="utf-8") as f:
                f.write("\n".join(found) + "\n")
            print(f"# wrote {fp}", file=sys.stderr)
    elif a.cmd == "extract":
        data = g.read(a.key)
        name = (normalize_path(a.key) if not a.key.lower().startswith("0x") else a.key + ".core")
        fp = os.path.join(a.out, "extracted", name.replace("/", os.sep))
        check_out_dir(fp, a.game)
        os.makedirs(os.path.dirname(fp), exist_ok=True)
        with open(fp, "wb") as f:
            f.write(data)
        print(f"{len(data)} bytes -> {fp}")
    elif a.cmd == "core":
        data = g.read(a.key)
        tn = TypeNames(a.types)
        dec = Decoder(tn) if a.types else None
        rows = []
        for off, tid, size, _g, body in read_core_objects(data):
            guid, _raw, exact = object_guid(dec, tn, tid, body)
            row = {"offset": off, "type_id": f"{tid:016X}", "type": tn.name(tid), "size": size,
                   "guid": guid if exact else f"{guid}?"}
            if a.decode and re.search(a.decode, row["type"]) and tid in tn.names:
                try:
                    row["fields"] = dec.object(row["type"], body)
                except (ValueError, struct.error, IndexError) as e:
                    row["fields"] = {"_error": str(e)}
            if a.strings:
                row["strings"] = embedded_strings(body)[:12]
            rows.append(row)
        if a.json:
            print(json.dumps(rows, indent=1))
        else:
            for r in rows:
                s = f"{r['offset']:8d} {r['type_id']} {r['type']:<48} {r['size']:8d} {r['guid']}"
                if a.strings and r["strings"]:
                    s += "  " + " | ".join(r["strings"])
                print(s)
                if "fields" in r:
                    print("          " + json.dumps(r["fields"]))
    elif a.cmd == "prefetch":
        files, sizes = parse_prefetch(g.read(PREFETCH_PATH))
        for i, fn in enumerate(files):
            print(f"{sizes[i] if i < len(sizes) else -1:10d} {fn}")
        print(f"# {len(files)} paths", file=sys.stderr)
    elif a.cmd == "scan":
        rx = re.compile(a.regex, re.I)
        tn = TypeNames(a.types)
        dec = Decoder(tn) if a.types else None
        files, sizes = parse_prefetch(g.read(PREFETCH_PATH))
        cands = sorted({normalize_path(p) for p in files + read_path_lists(a.lists) if rx.search(p)})
        idx = g.index()
        fp = os.path.join(a.out, os.path.basename(a.output))
        n_ok = n_missing = n_bad = 0
        with open(fp, "w", encoding="utf-8") as out:
            for p in cands:
                h = path_hash(p)
                locs = idx.get(h)
                if not locs:
                    n_missing += 1
                    continue
                rec = {"path": p, "hash": f"{h:016x}", "archives": [x[0] for x in locs], "size": locs[0][2]}
                try:
                    data = g.read(h)
                    objs = []
                    for off, tid, size, _g, body in read_core_objects(data):
                        guid, raw, exact = object_guid(dec, tn, tid, body)
                        objs.append({"type": tn.name(tid), "type_id": f"{tid:016X}", "size": size,
                                     "guid": guid, "guid_raw": raw, "guid_exact": exact,
                                     "strings": embedded_strings(body)[:40]})
                    rec["objects"] = objs
                    n_ok += 1
                except (ValueError, KeyError, OSError) as e:
                    rec["error"] = str(e)
                    n_bad += 1
                out.write(json.dumps(rec) + "\n")
        print(f"# {n_ok} parsed, {n_bad} failed, {n_missing} not in archives -> {fp}", file=sys.stderr)
    elif a.cmd == "typecheck":
        tn = TypeNames(a.types)
        with open(a.rtti, encoding="utf-8") as f:
            exe = json.load(f)
        classes = {k for k, v in tn.decl.items() if isinstance(v, dict) and v.get("type") == "class"}
        both = classes & set(exe)
        print(f"type JSON classes: {len(classes)}; ds.exe compound types: {len(exe)}; in both: {len(both)}")
        print("only in JSON:", sorted(classes - set(exe))[:40])
        print("only in ds.exe:", sorted(set(exe) - classes)[:40])
        bad_base = [k for k in sorted(both)
                    if [b["name"] for b in tn.decl[k].get("bases", [])] != exe[k].get("bases", [])]
        print(f"base-class lists that differ: {len(bad_base)}", bad_base[:20])
        size_diff = [k for k in sorted(both) if "size" in tn.decl[k] and tn.decl[k]["size"] != exe[k]["size"]]
        print(f"sizes that differ (where the JSON has one): {len(size_diff)}", size_diff[:20])
    elif a.cmd == "grep":
        rx = re.compile(a.regex, re.I)
        files, _ = parse_prefetch(g.read(PREFETCH_PATH))
        for p in sorted(set(files) | set(read_path_lists(a.lists))):
            if rx.search(p):
                print(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
