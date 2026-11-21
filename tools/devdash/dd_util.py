"""devdash helpers: read-only file access, redaction, time (Python stdlib only).

Read rules (docs/devdash.md "Read rules"): open, read and close at once; never hold a
handle (the mod, supervisor and core replace files with .tmp + MoveFileExW, which fails
while any reader holds the target); never ask for write access; tail big files. On Windows
every read opens with FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE (open_shared):
Python's open() leaves out FILE_SHARE_DELETE, so a delete or a replace that met one of its
reads would fail (a restore reported INCOMPLETE, a marker file left stale).
"""
import datetime
import json
import os
import re
import sys
import time

TAIL_BYTES = 256 * 1024

if sys.platform == 'win32':
    import ctypes
    import msvcrt
    from ctypes import wintypes

    _k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _CreateFileW = _k32.CreateFileW
    _CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                             wintypes.DWORD, wintypes.HANDLE)
    _CreateFileW.restype = wintypes.HANDLE
    _CloseHandle = _k32.CloseHandle
    _CloseHandle.argtypes = (wintypes.HANDLE,)
    _INVALID = wintypes.HANDLE(-1).value
    _GENERIC_READ, _SHARE_ALL, _OPEN_EXISTING, _NORMAL = 0x80000000, 0x7, 3, 0x80

    def open_shared(path):
        """A read-only binary file object that never blocks a delete, rename or replace of the file."""
        h = _CreateFileW(os.fspath(path), _GENERIC_READ, _SHARE_ALL, None, _OPEN_EXISTING, _NORMAL, None)
        if h is None or h == _INVALID:
            err = ctypes.get_last_error()
            raise OSError(None, ctypes.FormatError(err), os.fspath(path), err)
        try:
            fd = msvcrt.open_osfhandle(h, os.O_RDONLY | getattr(os, 'O_BINARY', 0))
        except OSError:
            _CloseHandle(h)
            raise
        return os.fdopen(fd, 'rb')
else:
    def open_shared(path):
        return open(path, 'rb')


def now_ms():
    return int(time.time() * 1000)


def read_bytes(path, limit=None):
    """Whole file (or its last `limit` bytes) in one open/read/close. None when missing or locked."""
    try:
        with open_shared(path) as f:
            if limit is not None:
                f.seek(0, 2)
                size = f.tell()
                if size > limit:
                    f.seek(size - limit)
                else:
                    f.seek(0)
            else:
                f.seek(0)
            return f.read()
    except OSError:
        return None


def read_text(path, limit=None):
    b = read_bytes(path, limit)
    return None if b is None else b.decode('utf-8', 'replace')


def read_head(path, n=4096):
    try:
        with open_shared(path) as f:
            return f.read(n).decode('utf-8', 'replace')
    except OSError:
        return None


def first_line(path):
    h = read_head(path, 256)
    return None if h is None else h.split('\n')[0].strip()


def read_json(path):
    t = read_text(path)
    if t is None:
        return None
    try:
        return json.loads(t)
    except ValueError:
        return None


def stat_key(path):
    try:
        st = os.stat(path)
        return (st.st_size, st.st_mtime_ns)
    except OSError:
        return None


def mtime_ms(path):
    try:
        return int(os.stat(path).st_mtime * 1000)
    except OSError:
        return None


def read_lines_from(path, after=None, tail_lines=300, max_bytes=TAIL_BYTES):
    """Incremental line reader. after=None: the last `tail_lines` lines (from the last max_bytes).
    after=offset: the complete lines added since offset. A file smaller than offset was truncated
    or recreated (the mod log is reopened "w" per game start): reset and send the tail again.
    A trailing line without '\\n' is not sent yet (partial write); the offset stops before it."""
    try:
        with open_shared(path) as f:
            f.seek(0, 2)
            size = f.tell()
            reset = False
            if after is not None and (after > size or after < 0):
                after, reset = None, True
            if after is None:
                start = max(0, size - max_bytes)
            else:
                start = after
                if size - start > max_bytes:
                    start, reset = size - max_bytes, True
            f.seek(start)
            data = f.read(size - start)
    except OSError:
        return None
    truncated_head = False
    if start > 0 and (after is None or reset):
        nl = data.find(b'\n')
        data = data[nl + 1:] if nl >= 0 else b''
        truncated_head = True
    end = data.rfind(b'\n')
    complete = data[:end + 1] if end >= 0 else b''
    offset = size - (len(data) - len(complete))
    lines = complete.decode('utf-8', 'replace').split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    lines = [ln.rstrip('\r') for ln in lines]
    if after is None or reset:
        if len(lines) > tail_lines:
            lines = lines[-tail_lines:]
            truncated_head = True
    return {'offset': offset, 'size': size, 'reset': reset or after is None, 'lines': lines,
            'truncated_head': truncated_head}


def local_stamp_ms(stamp):
    """'20261121-075508' (local, naive) -> epoch ms."""
    try:
        return int(time.mktime(time.strptime(stamp, '%Y%m%d-%H%M%S')) * 1000)
    except (ValueError, OverflowError):
        return None


def local_iso_ms(iso):
    """'2026-11-21T07:55:08.434' (local, naive) -> epoch ms."""
    try:
        frac = 0.0
        if '.' in iso:
            iso, f = iso.split('.', 1)
            frac = float('0.' + re.sub(r'\D.*$', '', f) or '0')
        return int((time.mktime(time.strptime(iso, '%Y-%m-%dT%H:%M:%S')) + frac) * 1000)
    except (ValueError, OverflowError):
        return None


def utc_iso_ms(iso):
    """'2026-11-21T13:50:16.832Z' -> epoch ms."""
    if not isinstance(iso, str):
        return None
    try:
        s = iso.replace('Z', '+00:00')
        return int(datetime.datetime.fromisoformat(s).timestamp() * 1000)
    except ValueError:
        return None


def utc_iso_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3] + 'Z'


def today_prefix():
    return time.strftime('%Y%m%d')


class Redactor:
    """API output never carries the Windows user path, the save account folder digits or ids.
    The page is local only, but screenshots and logs get pasted into chats and issues."""

    def __init__(self, home=None):
        home = home if home is not None else os.path.expanduser('~')
        self.rules = []
        if home and len(home) > 3:
            variants = {home, home.replace('\\', '/'), home.replace('\\', '\\\\')}
            if home[1] == ':':  # Git Bash / MSYS (/c/Users/<name>) and WSL (/mnt/c/Users/<name>)
                rest = home[2:].replace('\\', '/')
                variants |= {'/' + home[0].lower() + rest, '/mnt/' + home[0].lower() + rest}
            for v in sorted(variants, key=len, reverse=True):
                self.rules.append((re.compile(re.escape(v), re.IGNORECASE), '%USERPROFILE%'))
            # the profile folder's name after any Users\ in a form not listed above (another drive, doubled slashes)
            name = os.path.basename(home.rstrip('\\/'))
            if len(name) > 1:
                self.rules.append((re.compile(r'([\\/]+Users[\\/]+)' + re.escape(name) + r'(?![A-Za-z0-9_.-])',
                                              re.IGNORECASE), r'\1<user>'))
        self.rules += [
            (re.compile(r'(DeathStranding(?:DC)?[\\/]+)\d{5,}', re.IGNORECASE), r'\1<account>'),
            (re.compile(r'\bepic_[0-9a-fA-F]{8,}'), 'epic_<id>'),
            (re.compile(r'\b7656119\d{10}\b'), '<steamid>'),
            (re.compile(r'([\\/]userdata[\\/]+)\d+', re.IGNORECASE), r'\1<account>'),
        ]

    def text(self, s):
        for rx, rep in self.rules:
            s = rx.sub(rep, s)
        return s

    def obj(self, o):
        if isinstance(o, str):
            return self.text(o)
        if isinstance(o, dict):
            return {(self.text(k) if isinstance(k, str) else k): self.obj(v) for k, v in o.items()}
        if isinstance(o, (list, tuple)):
            return [self.obj(v) for v in o]
        return o


REDACT = Redactor()


def clip(s, n):
    """Redact, then cut: a cut first could leave half a user path that no rule matches any more."""
    if s is None:
        return None
    return REDACT.text(str(s))[:n]
