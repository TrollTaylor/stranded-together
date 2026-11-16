"""Dump Decima's compound RTTI types from ds.exe into .cache/rtti.json.

A compound type descriptor (e.g. DSPlayerEntity at 0x144BC6E00) looks like:
  +0x00 i32 id (-1)   +0x04 u8 kind (4 = compound)   +0x06 u8 base count
  +0x14 u32 size      +0x18 u32 alignment
  +0x20 constructor   +0x28 destructor
  +0x40 const char* name
  +0x60 bases: array of {RTTI* type; u32 offset} (16 bytes each)
Usage: python tools/re/rtti.py [--grep REGEX] [--derives NAME]
"""
import json, os, re, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from tools.re.dsre import DS, CACHE  # noqa: E402

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_<>:, ]{1,120}$")


def build(ds):
    types = {}
    for sec in (".data", ".rdata"):
        try:
            _, va, vs, _, _ = ds.sec(sec)
        except KeyError:
            continue
        blob = ds.rd(va, vs)
        for off in range(0, len(blob) - 0x70, 8):
            if blob[off:off + 4] != b"\xff\xff\xff\xff" or blob[off + 4] != 4:
                continue
            t = va + off
            name_p = ds.q(t + 0x40)
            if not ds.in_image(name_p):
                continue
            name = ds.cstr(name_p, 128)
            if not IDENT.match(name):
                continue
            nb = blob[off + 6]
            bases = []
            bp = ds.q(t + 0x60)
            if nb and ds.in_image(bp):
                for i in range(min(nb, 16)):
                    b = ds.q(bp + 16 * i)
                    if ds.in_image(b) and ds.in_image(ds.q(b + 0x40)):
                        bases.append(ds.cstr(ds.q(b + 0x40), 128))
            types.setdefault(name, {"va": hex(t), "size": ds.d(t + 0x14), "bases": bases})
    return types


def load():
    path = os.path.join(CACHE, "rtti.json")
    if os.path.exists(path):
        return json.load(open(path))
    t = build(DS.load())
    json.dump(t, open(path, "w"), indent=0, sort_keys=True)
    return t


def derives(types, name, root):
    seen = set()
    stack = [name]
    while stack:
        n = stack.pop()
        if n == root:
            return True
        if n in seen or n not in types:
            continue
        seen.add(n)
        stack += types[n]["bases"]
    return False


if __name__ == "__main__":
    types = load()
    args = sys.argv[1:]
    rx = re.compile(args[args.index("--grep") + 1], re.I) if "--grep" in args else None
    root = args[args.index("--derives") + 1] if "--derives" in args else None
    print(f"{len(types)} compound types", file=sys.stderr)
    for n, v in sorted(types.items()):
        if rx and not rx.search(n):
            continue
        if root and not derives(types, n, root):
            continue
        print(f"{n:50s} {v['va']} size=0x{v['size']:X} bases={','.join(v['bases'])}")
