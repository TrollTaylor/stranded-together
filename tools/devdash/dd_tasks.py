"""Posted tasks: the devdash status protocol (docs/devdash.md "Posting progress").

One append-only JSONL file per task: <state>\\status\\<id>.jsonl (state = <repo>\\build-devdash, or
the ST_DEVDASH_DIR environment variable). Every record is one JSON object on one line, written in
one call: {"v":1, "t":"<ISO UTC>", "op":"open|progress|item|log|beat|close", ...}. Appends never
conflict with a reader (a .tmp + rename would). Readers skip a partial last line and unknown ops.
"""
import copy
import json
import os
import re
import threading
import time

from dd_util import open_shared, utc_iso_ms, utc_iso_now

TASK_ID = re.compile(r'^[a-z0-9][a-z0-9._-]{0,63}$')
KINDS = ('test', 'batch', 'research', 'build', 'other')
ITEM_STATES = ('queued', 'running', 'pass', 'fail', 'skip')
FINAL_STATES = ('pass', 'fail', 'done', 'cancelled')
STALE_MIN_S = 900
MAX_LOG = 500
MAX_RECORD = 8192


def status_dir(state_dir):
    return os.path.join(state_dir, 'status')


def new_task(tid):
    return {'id': tid, 'kind': 'other', 'title': tid, 'status': 'running', 'done': None, 'total': None,
            'parent': None, 'owner': None, 'pid': None, 'heartbeat_s': None, 'started': None, 'updated': None,
            'note': None, 'summary': None, 'items': {}, 'item_order': [], 'log': [], 'bad_lines': 0}


def apply(task, r):
    """Fold one record into a task. Returns the (possibly new) task."""
    op = r.get('op')
    t = utc_iso_ms(r.get('t'))
    closed = task['status'] in FINAL_STATES
    if op == 'open':
        if closed:  # the id is reused (a new batch under the same id): start over, keep recent logs
            keep_log = task['log'][-50:]
            task = new_task(task['id'])
            task['log'] = keep_log
        for k in ('kind', 'title', 'parent', 'owner', 'note'):
            if isinstance(r.get(k), str):
                task[k] = r[k][:300]
        for k in ('total', 'pid', 'heartbeat_s'):
            if isinstance(r.get(k), (int, float)):
                task[k] = r[k]
        task['status'] = 'running'
        task['started'] = t
    elif op == 'log':
        task['log'].append({'t': t, 'level': r.get('level') if r.get('level') in ('info', 'warn', 'error') else 'info',
                            'msg': str(r.get('msg', ''))[:2000]})
        if len(task['log']) > MAX_LOG:
            del task['log'][:len(task['log']) - MAX_LOG]
    elif closed:
        return task  # after close only logs (and a new open) count
    elif op == 'progress':
        for k in ('done', 'total'):
            if isinstance(r.get(k), (int, float)):
                task[k] = r[k]
        if isinstance(r.get('pid'), (int, float)):  # 0: no owner process for now (a batch between its parts)
            task['pid'] = r['pid'] or None
        if isinstance(r.get('note'), str):
            task['note'] = r['note'][:300]
        if isinstance(r.get('status'), str) and r['status'] not in FINAL_STATES:
            task['status'] = r['status'][:20]
    elif op == 'item':
        key = str(r.get('key', ''))[:120]
        if key:
            it = task['items'].get(key)
            if it is None:
                it = task['items'][key] = {'key': key, 'title': key, 'status': 'queued', 'started': None,
                                           'ended': None}
                task['item_order'].append(key)
            if r.get('status') in ITEM_STATES:
                if r['status'] == 'running' and it['status'] != 'running':
                    it['started'] = t
                if r['status'] in ('pass', 'fail', 'skip'):
                    it['ended'] = t
                it['status'] = r['status']
            for k in ('title', 'run_dir', 'detail', 'scenario', 'runs_root'):
                if isinstance(r.get(k), str):
                    it[k] = r[k][:1000]
            if isinstance(r.get('verdict'), dict):
                it['verdict'] = r['verdict']
    elif op == 'close':
        task['status'] = r.get('status') if r.get('status') in FINAL_STATES else 'done'
        if isinstance(r.get('summary'), str):
            task['summary'] = r['summary'][:2000]
        task['ended'] = t
    if task['started'] is None and t:
        task['started'] = t
    if t:
        task['updated'] = t
    return task


class TaskStore:
    def __init__(self, state_dir, procs, now=None):
        self.dir = status_dir(state_dir)
        self.procs = procs
        self._now = now
        # One lock for every fold and every brief: the HTTP server answers requests on several threads at once
        # (overview and task detail polls), and a fold changes the cached task in place. Briefs hand out copies, so
        # serialising them never meets a dict another thread is changing.
        self._lock = threading.RLock()
        self._files = {}  # id -> (offset, size, task)

    def now(self):
        return self._now() if self._now else time.time()

    def _fold(self, tid):
        with self._lock:
            return self._fold_locked(tid)

    def _fold_locked(self, tid):
        p = os.path.join(self.dir, tid + '.jsonl')
        try:
            size = os.path.getsize(p)
        except OSError:
            return None
        c = self._files.get(tid)
        if c and c[1] == size:
            return c[2]
        offset, task = (c[0], c[2]) if c and size >= c[1] else (0, new_task(tid))
        try:
            with open_shared(p) as f:
                f.seek(offset)
                data = f.read(size - offset)
        except OSError:
            return c[2] if c else None
        end = data.rfind(b'\n')
        complete = data[:end + 1] if end >= 0 else b''
        for ln in complete.split(b'\n'):
            ln = ln.strip()
            if not ln:
                continue
            try:
                r = json.loads(ln.decode('utf-8', 'replace'))
            except ValueError:
                task['bad_lines'] += 1
                continue
            if isinstance(r, dict):
                task = apply(task, r)
        offset += len(complete)
        self._files[tid] = (offset, offset, task)
        return task

    def ids(self):
        try:
            return sorted(f[:-6] for f in os.listdir(self.dir) if f.endswith('.jsonl') and TASK_ID.match(f[:-6]))
        except OSError:
            return []

    def brief(self, task, with_items=False):
        with self._lock:
            return self._brief_locked(task, with_items)

    def _brief_locked(self, task, with_items):
        items = [task['items'][k] for k in task['item_order']]
        n = {s: sum(1 for i in items if i['status'] == s) for s in ITEM_STATES}
        done = task['done'] if task['done'] is not None else (n['pass'] + n['fail'] + n['skip'] if items else None)
        total = task['total']
        if items:
            total = max(total or 0, len(items))
        status = task['status']
        if status not in FINAL_STATES:
            hb = task['heartbeat_s'] or 0
            silent = self.now() - (task['updated'] or 0) / 1000.0
            if silent > max(3 * hb, STALE_MIN_S) or (task['pid'] and not self.procs.alive(int(task['pid']))):
                status = 'stale'
        b = {'id': task['id'], 'kind': task['kind'], 'title': task['title'], 'status': status,
             'done': done, 'total': total,
             'pct': round(100.0 * done / total, 1) if total and done is not None else None,
             'parent': task['parent'], 'owner': task['owner'], 'started': task['started'],
             'updated': task['updated'], 'ended': task.get('ended'), 'note': task['note'], 'summary': task['summary'],
             'items_count': n, 'last_log': task['log'][-1] if task['log'] else None}
        cur = next((i for i in items if i['status'] == 'running'), None)
        b['current'] = cur['title'] if cur else None
        if with_items:
            b['items'] = copy.deepcopy(items)  # the caller may add to them (devdash links runs): never the cache's own
        if b['last_log']:
            b['last_log'] = dict(b['last_log'])
        return b

    def list(self, include_old=False, with_items=False):
        out = []
        for tid in self.ids():
            with self._lock:
                t = self._fold_locked(tid)
                if not t or t['started'] is None:
                    continue
                b = self._brief_locked(t, with_items)
            # finished and stale tasks (an abandoned batch: its owner killed, or a batch in parts never continued)
            # leave the overview after a day without a record
            if not include_old and b['status'] in FINAL_STATES + ('stale',) and self.now() - (b['updated'] or 0) / 1000.0 > 86400:
                continue
            out.append(b)
        # running tasks first; a stale task runs nothing more, so it sorts with the finished ones by its last record
        # instead of sitting above every newer batch for a day
        out.sort(key=lambda b: (0 if b['status'] == 'running' else 1, -(b['updated'] or 0)))
        return out

    def detail(self, tid):
        if not TASK_ID.match(tid or ''):
            return None
        with self._lock:
            t = self._fold_locked(tid)
            if not t:
                return None
            b = self._brief_locked(t, with_items=True)
            b['log'] = [dict(x) for x in t['log'][-MAX_LOG:]]
        b['children'] = [c for c in self.list(include_old=True) if c['parent'] == tid]
        return b


def post(state_dir, tid, records):
    """Append records (dicts with 'op') to <state>\\status\\<id>.jsonl, one write per record."""
    if not TASK_ID.match(tid):
        raise ValueError('task id must match %s' % TASK_ID.pattern)
    d = status_dir(state_dir)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, tid + '.jsonl')
    for r in records:
        rec = {'v': 1, 't': utc_iso_now()}
        rec.update(r)
        rec.setdefault('id', tid)
        line = (json.dumps(rec, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')
        if len(line) > MAX_RECORD:  # keep records small: cut long strings
            rec = {k: (v[:1500] if isinstance(v, str) else v) for k, v in rec.items()}
            line = (json.dumps(rec, ensure_ascii=False, separators=(',', ':')) + '\n').encode('utf-8')
        for attempt in range(5):
            try:
                with open(p, 'ab') as f:
                    f.write(line)
                break
            except OSError:
                if attempt == 4:
                    raise
                time.sleep(0.05)
