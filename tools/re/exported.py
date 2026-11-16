"""Extract Decima exported script symbols: (name, signature-ish string, function pointer)."""
import re, struct, json, sys
import capstone
from dsre import DS
ds = DS.load()
_, tva, tvs, _, _ = ds.sec(".text")
t = ds.rd(tva, tvs)
out = []
# pattern: lea rax,[rip+F]; (mov dword [rsp+28],imm)?; ...; lea r8,[rip+S2]; mov rcx,..; lea rdx,[rip+NAME]; call
for m in re.finditer(rb"\x48\x8d\x05(....)(?:.{0,40}?)\x4c\x8d\x05(....)(?:.{0,6}?)\x48\x8d\x15(....)\xe8", t, re.S):
    o = m.start()
    f = tva + o + 7 + struct.unpack("<i", m.group(1))[0]
    o2 = m.start(2) - 3
    s2 = tva + o2 + 7 + struct.unpack("<i", m.group(2))[0]
    o3 = m.start(3) - 3
    nm = tva + o3 + 7 + struct.unpack("<i", m.group(3))[0]
    if not ds.in_text(f) or ds.in_text(nm):
        continue
    name = ds.cstr(nm, 200)
    sig = ds.cstr(s2, 300) if not ds.in_text(s2) else ""
    if not re.fullmatch(r"[A-Za-z_][\w:<>, ]{1,120}", name or ""):
        continue
    out.append({"name": name, "sig": sig, "func": hex(f), "site": hex(tva + o)})
json.dump(out, open(".cache/exported.json", "w"), indent=0)
print(len(out))
