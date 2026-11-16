"""Tiny client for the GhidraMCP plugin HTTP server (http://127.0.0.1:8080).

  python gh.py dec 0x142374400            # decompile function at address
  python gh.py dis 0x142374400            # disassemble function
  python gh.py xto 0x142374400            # xrefs to address
  python gh.py ren 0x142374400 NewName    # rename function at address
  python gh.py cmt 0x142374400 "comment"  # set decompiler comment
  python gh.py fn  0x142374400            # function containing / at address
"""
import sys
import urllib.parse
import urllib.request

BASE = "http://127.0.0.1:8080/"


def get(ep, **params):
    url = BASE + ep + ("?" + urllib.parse.urlencode(params) if params else "")
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read().decode("utf-8", "replace")


def post(ep, **data):
    body = urllib.parse.urlencode(data).encode()
    with urllib.request.urlopen(urllib.request.Request(BASE + ep, data=body), timeout=300) as r:
        return r.read().decode("utf-8", "replace")


def main(argv):
    cmd, args = argv[1], argv[2:]
    if cmd == "dec":
        print(get("decompile_function", address=args[0]))
    elif cmd == "dis":
        print(get("disassemble_function", address=args[0]))
    elif cmd == "xto":
        print(get("xrefs_to", address=args[0], offset=0, limit=int(args[1]) if len(args) > 1 else 100))
    elif cmd == "ren":
        print(post("rename_function_by_address", function_address=args[0], new_name=args[1]))
    elif cmd == "cmt":
        print(post("set_decompiler_comment", address=args[0], comment=args[1]))
    elif cmd == "fn":
        print(get("get_function_by_address", address=args[0]))
    elif cmd == "raw":
        print(get(args[0], **dict(a.split("=", 1) for a in args[1:])))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
