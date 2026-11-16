"""Static analysis helpers for ds.exe (Death Stranding Director's Cut).

Builds a cached index of: section map, .pdata function table, ASCII strings,
RIP-relative LEA/MOV references from .text, and absolute pointers in data.

Usage as a library:
    from dsre import DS
    ds = DS.load()
    ds.find_str(b"GetLocalDSPlayerEntity")
    ds.code_refs(va)            # instructions that reference va RIP-relatively
    ds.data_refs(va)            # qword slots in .rdata/.data that hold va
    ds.func_of(va)              # (start, end) of containing function via .pdata
    ds.dis(va, n)               # capstone disassembly
"""
import os, sys, re, pickle, struct, bisect
import numpy as np
import pefile

GAME = r"C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT\ds.exe"
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".cache")


class DS:
    def __init__(self, path=GAME):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.raw = open(path, "rb").read()
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b"\0").decode()
            self.secs.append((name, self.base + s.VirtualAddress, s.Misc_VirtualSize,
                              s.PointerToRawData, s.SizeOfRawData))
        # memory image (virtual layout) for convenient slicing
        size = self.pe.OPTIONAL_HEADER.SizeOfImage
        self.img = bytearray(size)
        self.img[: self.pe.OPTIONAL_HEADER.SizeOfHeaders] = self.raw[: self.pe.OPTIONAL_HEADER.SizeOfHeaders]
        for name, va, vs, ro, rs in self.secs:
            n = min(vs, rs)
            self.img[va - self.base: va - self.base + n] = self.raw[ro: ro + n]
        self.img = bytes(self.img)

    # ---------------- basic access
    def sec(self, name):
        for s in self.secs:
            if s[0] == name:
                return s
        raise KeyError(name)

    def rd(self, va, n):
        o = va - self.base
        return self.img[o:o + n]

    def q(self, va):
        return struct.unpack_from("<Q", self.img, va - self.base)[0]

    def d(self, va):
        return struct.unpack_from("<I", self.img, va - self.base)[0]

    def i32(self, va):
        return struct.unpack_from("<i", self.img, va - self.base)[0]

    def cstr(self, va, maxn=512):
        b = self.rd(va, maxn)
        return b.split(b"\0", 1)[0].decode("latin1")

    def in_image(self, va):
        return self.base <= va < self.base + len(self.img)

    def in_text(self, va):
        _, tva, tvs, _, _ = self.sec(".text")
        return tva <= va < tva + tvs

    # ---------------- index
    @classmethod
    def load(cls, path=GAME):
        ds = cls(path)
        os.makedirs(CACHE, exist_ok=True)
        st = os.stat(path)
        key = os.path.join(CACHE, f"idx_{st.st_size}_{int(st.st_mtime)}.pkl")
        if os.path.exists(key):
            with open(key, "rb") as f:
                ds.__dict__.update(pickle.load(f))
        else:
            ds._build()
            with open(key, "wb") as f:
                pickle.dump({k: getattr(ds, k) for k in ("funcs_s", "funcs_e", "lea_src", "lea_dst",
                                                         "dp_src", "dp_dst", "call_src", "call_dst")}, f)
        return ds

    def _build(self):
        # .pdata
        name, pva, pvs, _, _ = self.sec(".pdata")
        arr = np.frombuffer(self.rd(pva, pvs - pvs % 12), dtype="<u4").reshape(-1, 3)
        arr = arr[arr[:, 0] != 0]
        order = np.argsort(arr[:, 0])
        self.funcs_s = (arr[order, 0].astype(np.int64) + self.base)
        self.funcs_e = (arr[order, 1].astype(np.int64) + self.base)
        # RIP-relative refs in .text: scan for ModRM with mod=00 rm=101 after common opcodes.
        _, tva, tvs, _, _ = self.sec(".text")
        t = self.rd(tva, tvs)
        src, dst = [], []
        # lea/mov/cmp etc with REX.W prefix:  (48|4C) (8D|8B|89|3B|39|03|2B|33|0B|23|85) modrm disp32
        rx = re.compile(rb"[\x48\x4c](?:[\x8d\x8b\x89\x3b\x39\x03\x2b\x33\x0b\x23\x85\xc7\x63])[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)
        for m in rx.finditer(t):
            o = m.start()
            disp = struct.unpack_from("<i", t, o + 3)[0]
            ilen = 7 + (4 if t[o + 1] == 0xC7 else 0)
            src.append(tva + o)
            dst.append(tva + o + ilen + disp)
        # non-REX: 8B/89/8D/3B/39/FF(call/jmp [rip]) and SSE movss/movaps etc skipped
        rx2 = re.compile(rb"(?<![\x48\x4c\x49\x4d\x0f\x66\xf3\xf2])[\x8b\x89\x8d\x3b\x39\x83\x80\xc6\xc7\xff\x0f\xb6][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)
        for m in rx2.finditer(t):
            o = m.start()
            op = t[o]
            extra = {0x83: 1, 0x80: 1, 0xC6: 1, 0xC7: 4}.get(op, 0)
            disp = struct.unpack_from("<i", t, o + 2)[0]
            src.append(tva + o)
            dst.append(tva + o + 6 + extra + disp)
        # SSE loads: (F3|F2|66)? 0F (10|11|28|29|2E|2F|59|58|5C|5E) modrm-rip
        rx3 = re.compile(rb"\x0f[\x10\x11\x28\x29\x2e\x2f\x58\x59\x5c\x5e\x54\x57\x6f\x7f\xc6][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)
        for m in rx3.finditer(t):
            o = m.start()
            disp = struct.unpack_from("<i", t, o + 2)[0]
            src.append(tva + o)
            dst.append(tva + o + 6 + disp)
        src = np.array(src, dtype=np.int64)
        dst = np.array(dst, dtype=np.int64)
        ok = (dst >= self.base) & (dst < self.base + len(self.img))
        src, dst = src[ok], dst[ok]
        order = np.argsort(dst, kind="stable")
        self.lea_src, self.lea_dst = src[order], dst[order]
        # E8 calls / E9 jmps
        cs, cd = [], []
        for m in re.finditer(rb"[\xe8\xe9]", t):
            o = m.start()
            if o + 5 > len(t):
                break
            disp = struct.unpack_from("<i", t, o + 1)[0]
            target = tva + o + 5 + disp
            if tva <= target < tva + tvs:
                cs.append(tva + o)
                cd.append(target)
        cs = np.array(cs, dtype=np.int64)
        cd = np.array(cd, dtype=np.int64)
        # keep only calls whose target is a known function start
        fs = set(self.funcs_s.tolist())
        keep = np.array([x in fs for x in cd.tolist()], dtype=bool)
        cs, cd = cs[keep], cd[keep]
        order = np.argsort(cd, kind="stable")
        self.call_src, self.call_dst = cs[order], cd[order]
        # absolute qword pointers in .rdata/.data
        ps, pd = [], []
        for nm in (".rdata", ".data"):
            _, va, vs, _, rs = self.sec(nm)
            n = min(vs, rs) // 8 * 8
            a = np.frombuffer(self.rd(va, n), dtype="<u8").astype(np.int64)
            idx = np.nonzero((a >= self.base) & (a < self.base + len(self.img)))[0]
            ps.append(va + idx * 8)
            pd.append(a[idx])
        ps = np.concatenate(ps)
        pd = np.concatenate(pd)
        order = np.argsort(pd, kind="stable")
        self.dp_src, self.dp_dst = ps[order], pd[order]

    # ---------------- queries
    @staticmethod
    def _lookup(dst, src, va):
        lo = np.searchsorted(dst, va, "left")
        hi = np.searchsorted(dst, va, "right")
        return [int(x) for x in src[lo:hi]]

    def code_refs(self, va):
        return self._lookup(self.lea_dst, self.lea_src, va)

    def data_refs(self, va):
        return self._lookup(self.dp_dst, self.dp_src, va)

    def callers(self, va):
        return self._lookup(self.call_dst, self.call_src, va)

    def func_of(self, va):
        i = np.searchsorted(self.funcs_s, va, "right") - 1
        if i < 0:
            return None
        s, e = int(self.funcs_s[i]), int(self.funcs_e[i])
        if s <= va < e:
            return (s, e)
        return None

    def find_str(self, s, exact=True):
        """Return VAs of NUL-terminated occurrences of byte string s (exact=True requires a NUL before too)."""
        if isinstance(s, str):
            s = s.encode()
        out = []
        pat = (b"\0" + re.escape(s) + b"\0") if exact else re.escape(s)
        for m in re.finditer(pat, self.img):
            out.append(self.base + m.start() + (1 if exact else 0))
        return out

    def strings_matching(self, rx, minlen=4):
        if isinstance(rx, str):
            rx = rx.encode()
        out = []
        for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, self.img):
            if re.search(rx, m.group()):
                out.append((self.base + m.start(), m.group().decode()))
        return out

    def dis(self, va, n=40, stop_ret=True):
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = False
        code = self.rd(va, n * 15)
        out = []
        for ins in md.disasm(code, va):
            out.append(f"{ins.address:#x}: {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")
            if len(out) >= n or (stop_ret and ins.mnemonic in ("ret", "int3")):
                break
        return "\n".join(out)

    def dis_func(self, va, maxn=400):
        f = self.func_of(va)
        if not f:
            return self.dis(va, 60)
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        out = []
        for ins in md.disasm(self.rd(f[0], f[1] - f[0]), f[0]):
            out.append(f"{ins.address:#x}: {ins.mnemonic} {ins.op_str}")
            if len(out) >= maxn:
                break
        return "\n".join(out)

    def rva(self, va):
        return va - self.base

    def file_off(self, va):
        for name, sva, vs, ro, rs in self.secs:
            if sva <= va < sva + rs:
                return ro + (va - sva)
        return None

    def sig(self, va, n=24):
        """Return a hex byte pattern for va with RIP-relative disp32 wildcarded (best effort via capstone)."""
        import capstone
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        md.detail = True
        toks = []
        for ins in md.disasm(self.rd(va, n + 16), va):
            b = list(ins.bytes)
            wild = set()
            if ins.mnemonic in ("call", "jmp") and len(b) == 5 and b[0] in (0xE8, 0xE9):
                wild = {1, 2, 3, 4}
            elif "rip" in ins.op_str:
                # disp32 sits at disp_offset
                off = ins.disp_offset if hasattr(ins, "disp_offset") else None
                if off:
                    wild = {off, off + 1, off + 2, off + 3}
            toks += ["??" if i in wild else f"{x:02X}" for i, x in enumerate(b)]
            if len(toks) >= n:
                break
        return " ".join(toks)


if __name__ == "__main__":
    ds = DS.load()
    print("functions", len(ds.funcs_s), "riprefs", len(ds.lea_src), "calls", len(ds.call_src), "dataptrs", len(ds.dp_src))
