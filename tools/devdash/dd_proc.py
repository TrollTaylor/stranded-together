"""Process list without opening any process handle (CreateToolhelp32Snapshot).

devdash never opens ds.exe or the user's StrandedTogether.exe. The only handle it ever opens
is a PROCESS_QUERY_LIMITED_INFORMATION one on st_testenv_supervisor.exe, to read its
--run-dir (a run under a -BuildDir outside the repo). It never touches the supervisor's mutex
Local\\StrandedTogetherTestEnv: holding it would make the next hidden run refuse.
"""
import ctypes
import os
import threading
import time

_IS_WIN = os.name == 'nt'
SUPERVISOR = 'st_testenv_supervisor.exe'

if _IS_WIN:
    from ctypes import wintypes

    _k = ctypes.WinDLL('kernel32', use_last_error=True)
    _nt = ctypes.WinDLL('ntdll')
    _shell = ctypes.WinDLL('shell32')

    class _PE(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                    ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD),
                    ('cntThreads', wintypes.DWORD), ('th32ParentProcessID', wintypes.DWORD),
                    ('pcPriClassBase', ctypes.c_long), ('dwFlags', wintypes.DWORD),
                    ('szExeFile', ctypes.c_wchar * 260)]

    class _US(ctypes.Structure):
        _fields_ = [('Length', wintypes.USHORT), ('MaximumLength', wintypes.USHORT), ('Buffer', ctypes.c_void_p)]

    _k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _k.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _k.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PE)]
    _k.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PE)]
    _k.CloseHandle.argtypes = [wintypes.HANDLE]
    _k.OpenProcess.restype = wintypes.HANDLE
    _k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _k.LocalFree.argtypes = [ctypes.c_void_p]
    _nt.NtQueryInformationProcess.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.ULONG,
                                             ctypes.POINTER(wintypes.ULONG)]
    _shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
    _shell.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]


def _snapshot():
    if not _IS_WIN:
        return []
    snap = _k.CreateToolhelp32Snapshot(2, 0)  # TH32CS_SNAPPROCESS
    if not snap or snap == wintypes.HANDLE(-1).value:
        return []
    out = []
    try:
        e = _PE()
        e.dwSize = ctypes.sizeof(_PE)
        ok = _k.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            out.append((int(e.th32ProcessID), int(e.th32ParentProcessID), e.szExeFile))
            ok = _k.Process32NextW(snap, ctypes.byref(e))
    finally:
        _k.CloseHandle(snap)
    return out


def _cmdline(pid):
    if not _IS_WIN:
        return None
    h = _k.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION only
    if not h:
        return None
    try:
        need = wintypes.ULONG(0)
        _nt.NtQueryInformationProcess(h, 60, None, 0, ctypes.byref(need))  # ProcessCommandLineInformation
        buf = ctypes.create_string_buffer(max(need.value, 64))
        if _nt.NtQueryInformationProcess(h, 60, buf, len(buf), ctypes.byref(need)) != 0:
            return None
        us = _US.from_buffer(buf)
        return ctypes.wstring_at(us.Buffer, us.Length // 2) if us.Buffer else None
    finally:
        _k.CloseHandle(h)


def _argv(cmd):
    n = ctypes.c_int(0)
    p = _shell.CommandLineToArgvW(cmd, ctypes.byref(n))
    if not p:
        return []
    try:
        return [p[i] for i in range(n.value)]
    finally:
        _k.LocalFree(p)


class Procs:
    """Snapshot cached for `ttl` seconds; supervisor command lines cached per pid."""

    def __init__(self, ttl=1.0):
        self.ttl = ttl
        self._lock = threading.Lock()
        self._at = 0.0
        self._list = []
        self._sup_args = {}

    def list(self):
        with self._lock:
            if time.time() - self._at > self.ttl:
                try:
                    self._list = _snapshot()
                except OSError:
                    self._list = []
                self._at = time.time()
            return self._list

    def pids(self, exe):
        exe = exe.lower()
        return [p for p, _pp, n in self.list() if n.lower() == exe]

    def alive(self, pid):
        return any(p == pid for p, _pp, _n in self.list())

    def supervisors(self):
        """[{pid, run_dir|None, mode}] for every live st_testenv_supervisor.exe."""
        out = []
        live = set(self.pids(SUPERVISOR))
        with self._lock:
            for pid in list(self._sup_args):
                if pid not in live:
                    del self._sup_args[pid]
        for pid in sorted(live):
            with self._lock:
                info = self._sup_args.get(pid)
            if info is None:
                info = {'run_dir': None, 'mode': 'run'}
                cmd = _cmdline(pid)
                if cmd:
                    a = _argv(cmd)
                    for i, x in enumerate(a[:-1]):
                        if x == '--run-dir':
                            info['run_dir'] = a[i + 1]
                        elif x == '--restore':
                            info['run_dir'], info['mode'] = a[i + 1], 'restore'
                    if '--probe' in a:
                        info['mode'] = 'probe'
                with self._lock:
                    self._sup_args[pid] = info
            out.append(dict(info, pid=pid))
        return out


class FakeProcs(Procs):
    """For tests: a fixed process list and supervisor table."""

    def __init__(self, procs=(), sups=()):
        super().__init__()
        self._fixed = list(procs)
        self._sups = list(sups)

    def list(self):
        return self._fixed

    def supervisors(self):
        return [dict(s) for s in self._sups]
