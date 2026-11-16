"""Generate unique byte signatures (with RIP/call displacements wildcarded) for target functions."""
import re, sys, capstone
from dsre import DS
ds = DS.load()
_, tva, tvs, _, _ = ds.sec(".text")
text = ds.rd(tva, tvs)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True

def toks_for(va, nbytes):
    toks = []
    for ins in md.disasm(ds.rd(va, nbytes + 32), va):
        b = list(ins.bytes); wild = set()
        if ins.mnemonic in ("call", "jmp") and b[0] in (0xE8, 0xE9) and len(b) == 5: wild = {1,2,3,4}
        elif len(b) >= 2 and b[0] == 0x0F and 0x80 <= b[1] <= 0x8F: wild = {2,3,4,5}
        elif "rip" in ins.op_str and getattr(ins, "disp_offset", 0):
            o = ins.disp_offset; wild = {o, o+1, o+2, o+3}
        toks += [None if i in wild else x for i, x in enumerate(b)]
        if len(toks) >= nbytes: break
    return toks

def count(toks):
    rx = b"".join(b"." if t is None else re.escape(bytes([t])) for t in toks)
    return len(re.findall(rx, text, re.S)) if True else 0

def unique_sig(va):
    for n in (16, 24, 32, 40, 48, 64, 80, 96):
        toks = toks_for(va, n)
        rx = b"".join(b"." if t is None else re.escape(bytes([t])) for t in toks)
        hits = [m.start() for m in re.finditer(rx, text, re.S)]
        if len(hits) == 1 and tva + hits[0] == va:
            return " ".join("??" if t is None else f"{t:02X}" for t in toks)
    return None

targets = dict(a.split("=") for a in sys.argv[1:])
for name, a in targets.items():
    va = int(a, 16)
    print(f'{name:28s} rva=0x{va-ds.base:08X} sig="{unique_sig(va)}"')
