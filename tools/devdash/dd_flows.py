"""Claude Code workflow runs ("research"), read only.

Layout under %USERPROFILE%\\.claude\\projects\\<slug>\\ (slug = the repo path with every
non-alphanumeric character replaced by '-'; compared case-insensitively):
  <session>\\custom-title.json                                   {"customTitle": ...}
  <session>\\subagents\\workflows\\<runId>\\journal.jsonl            launched / started / result (no timestamps)
  <session>\\subagents\\workflows\\<runId>\\agent-<id>.jsonl         agent transcript, ISO 'timestamp' per record
  <session>\\subagents\\workflows\\<runId>\\agent-<id>.meta.json     description (= label), workflowPhase, model
  <session>\\workflows\\<runId>.json                               written once the workflow ENDS (authoritative)
  <session>\\workflows\\scripts\\<name>-<runId>.js                  meta.name / description / phases
The main transcript <session>.jsonl is never read (100 MB+).
"""
import glob
import json
import os
import re
import threading
import time

from dd_util import clip, read_head, read_json, read_text, stat_key, utc_iso_ms

RUNID = re.compile(r'^wf_[A-Za-z0-9-]{1,40}$')
AGENTID = re.compile(r'^a[0-9a-zA-Z]{6,40}$')
SESSION = re.compile(r'^[0-9a-fA-F-]{36}$')
LIVE_WINDOW_S = 15 * 60
AGENT_TAIL = 256 * 1024


def slug_of(path):
    return re.sub(r'[^A-Za-z0-9]', '-', path)


def default_projects_dir():
    return os.path.join(os.path.expanduser('~'), '.claude', 'projects')


def parse_meta(js_text):
    """name, description, phases[{title, detail}] from `export const meta = {...}`."""
    out = {'name': None, 'description': None, 'phases': []}
    if not js_text:
        return out
    m = re.search(r'export\s+const\s+meta\s*=\s*\{(.*?)\n\}', js_text, re.S)
    body = m.group(1) if m else js_text[:20000]
    q = r'([\'"`])((?:\\.|(?!\1).)*)\1'
    nm = re.search(r'\bname\s*:\s*' + q, body, re.S)
    ds = re.search(r'\bdescription\s*:\s*' + q, body, re.S)
    out['name'] = nm.group(2) if nm else None
    out['description'] = ds.group(2) if ds else None
    pm = re.search(r'\bphases\s*:\s*\[(.*?)\]', body, re.S)
    if pm:
        for obj in re.findall(r'\{(.*?)\}', pm.group(1), re.S):
            t = re.search(r'\btitle\s*:\s*' + q, obj, re.S)
            d = re.search(r'\bdetail\s*:\s*' + q, obj, re.S)
            if t:
                out['phases'].append({'title': t.group(2), 'detail': d.group(2) if d else ''})
    return out


def read_jsonl(path, limit=None):
    """Records of a JSONL file; a partial or broken line is skipped."""
    t = read_text(path, limit)
    if t is None:
        return []
    lines = t.split('\n')
    if limit is not None and len(t.encode('utf-8', 'replace')) >= limit and lines:
        lines = lines[1:]  # the first line of a tail is partial
    out = []
    for ln in lines:
        ln = ln.strip()
        if not ln:
            continue
        try:
            out.append(json.loads(ln))
        except ValueError:
            continue
    return out


def result_preview(r, n=600):
    if r is None:
        return None
    if isinstance(r, dict):
        for k in ('report', 'summary', 'result', 'text', 'message'):
            if isinstance(r.get(k), str):
                return clip(r[k], n)
        try:
            return clip(json.dumps(r, ensure_ascii=False), n)
        except (TypeError, ValueError):
            return clip(r, n)
    return clip(r, n)


def tool_summary(name, inp):
    if not isinstance(inp, dict):
        return ''
    for k in ('description', 'file_path', 'pattern', 'url', 'query', 'skill', 'action'):
        v = inp.get(k)
        if isinstance(v, str) and v:
            return clip(v, 200)
    v = inp.get('command')
    if isinstance(v, str):
        return clip(v.strip().split('\n')[0], 200)
    return ''


def fold_journal(records):
    """journal records -> (agents by agentId in start order, labels order). Retries: the same label
    started again; the older start never gets a result and counts as superseded."""
    agents, order, last_by_label = {}, [], {}
    for r in records:
        t = r.get('type')
        if t == 'started' and r.get('agentId'):
            aid = r['agentId']
            lab = r.get('label') or aid
            prev = last_by_label.get(lab)
            if prev and prev != aid and agents.get(prev, {}).get('state') != 'done':
                agents[prev]['state'] = 'superseded'
            agents[aid] = {'agentId': aid, 'label': lab, 'phase': r.get('phase'), 'state': 'running',
                           'result': None, 'seq': len(order)}
            order.append(aid)
            last_by_label[lab] = aid
        elif t == 'result' and r.get('agentId'):
            a = agents.get(r['agentId'])
            if a is None:
                a = agents[r['agentId']] = {'agentId': r['agentId'], 'label': r['agentId'], 'phase': None,
                                            'seq': len(order)}
                order.append(r['agentId'])
            a['state'] = 'done'
            a['result'] = r.get('result')
    return agents, order


def phase_progress(phase_titles, agents):
    """Per phase: started / done counts and a state (done|active|pending), plus an approximate %.
    Planned agent totals are not knowable (scripts loop over findings): the % is labelled approximate."""
    titles = list(phase_titles)
    for a in agents:
        if a.get('phase') and a['phase'] not in titles:
            titles.append(a['phase'])
    live = [a for a in agents if a['state'] != 'superseded']
    rows = []
    for t in titles:
        mine = [a for a in live if a.get('phase') == t]
        rows.append({'title': t, 'started': len(mine), 'done': sum(1 for a in mine if a['state'] == 'done'),
                     'running': sum(1 for a in mine if a['state'] in ('running', 'idle'))})
    last_started = max((i for i, r in enumerate(rows) if r['started']), default=-1)
    for i, r in enumerate(rows):
        if not r['started']:
            r['state'] = 'pending'
        elif r['running'] == 0 and i < last_started:
            r['state'] = 'done'
        else:
            r['state'] = 'active'
    if not rows:
        return rows, 0.0
    done_phases = sum(1 for r in rows if r['state'] == 'done')
    cur = next((r for r in rows if r['state'] == 'active'), None)
    frac = (cur['done'] / cur['started']) if cur and cur['started'] else 0.0
    return rows, round(100.0 * min(1.0, (done_phases + frac) / len(rows)), 1)


class FlowStore:
    def __init__(self, repo, projects_dir=None, now=None):
        self.repo = repo
        self.projects_dir = projects_dir or default_projects_dir()
        self._now = now
        self._lock = threading.Lock()
        self._final = {}     # path -> (statkey, json)
        self._astart = {}    # agent jsonl path -> start ms
        self._meta = {}      # path -> (statkey, meta)

    def now(self):
        return self._now() if self._now else time.time()

    def slug_dirs(self):
        slug = slug_of(self.repo).lower()
        out = []
        try:
            for d in os.listdir(self.projects_dir):
                dl = d.lower()
                if dl == slug or (len(dl) >= 40 and dl.endswith('…') and slug.startswith(dl[:-1])):
                    p = os.path.join(self.projects_dir, d)
                    if os.path.isdir(p):
                        out.append(p)
        except OSError:
            pass
        return out

    def _sessions(self):
        for sd in self.slug_dirs():
            try:
                names = os.listdir(sd)
            except OSError:
                continue
            for s in names:
                if SESSION.match(s) and os.path.isdir(os.path.join(sd, s)):
                    yield os.path.join(sd, s)

    def runs(self):
        """[(session_dir, runId)] for every workflow run of this repo."""
        out = []
        for sess in self._sessions():
            wd = os.path.join(sess, 'subagents', 'workflows')
            ids = set()
            try:
                ids |= {d for d in os.listdir(wd) if RUNID.match(d)}
            except OSError:
                pass
            try:
                ids |= {f[:-5] for f in os.listdir(os.path.join(sess, 'workflows'))
                        if f.endswith('.json') and RUNID.match(f[:-5])}
            except OSError:
                pass
            out += [(sess, r) for r in ids]
        return out

    def find(self, run_id):
        if not RUNID.match(run_id or ''):
            return None
        for sess, r in self.runs():
            if r == run_id:
                return sess
        return None

    def _cached_json(self, path):
        k = stat_key(path)
        if k is None:
            return None
        with self._lock:
            c = self._final.get(path)
            if c and c[0] == k:
                return c[1]
        j = read_json(path)
        if j is not None:
            with self._lock:
                self._final[path] = (k, j)
        return j

    def _script_meta(self, sess, run_id):
        files = glob.glob(os.path.join(glob.escape(os.path.join(sess, 'workflows', 'scripts')), '*-%s.js' % run_id))
        if not files:
            return parse_meta(None)
        p = files[0]
        k = stat_key(p)
        with self._lock:
            c = self._meta.get(p)
            if c and c[0] == k:
                return c[1]
        m = parse_meta(read_text(p, 256 * 1024))
        with self._lock:
            self._meta[p] = (k, m)
        return m

    def _agent_start(self, path):
        with self._lock:
            if path in self._astart:
                return self._astart[path]
        h = read_head(path, 16384) or ''
        start = None
        for ln in h.split('\n'):
            m = re.search(r'"timestamp"\s*:\s*"([^"]+)"', ln)
            if m:
                start = utc_iso_ms(m.group(1))
                break
        if start is not None:
            with self._lock:
                self._astart[path] = start
        return start

    @staticmethod
    def _session_title(sess):
        j = read_json(os.path.join(sess, 'custom-title.json')) or {}
        return j.get('customTitle') or os.path.basename(sess)[:8]

    def brief(self, sess, run_id, detail=False):
        final = self._cached_json(os.path.join(sess, 'workflows', run_id + '.json'))
        wdir = os.path.join(sess, 'subagents', 'workflows', run_id)
        journal = read_jsonl(os.path.join(wdir, 'journal.jsonl'))
        agents, order = fold_journal(journal)
        meta = self._script_meta(sess, run_id)
        now_s = self.now()
        latest_write = 0.0
        jk = stat_key(os.path.join(wdir, 'journal.jsonl'))
        if jk:
            latest_write = jk[1] / 1e9
        alist = []
        for aid in order:
            a = dict(agents[aid])
            ap = os.path.join(wdir, 'agent-%s.jsonl' % aid)
            sk = stat_key(ap)
            a['started'] = self._agent_start(ap) if sk else None
            a['last_activity'] = int(sk[1] / 1e6) if sk else None
            if sk:
                latest_write = max(latest_write, sk[1] / 1e9)
            if a['state'] == 'running' and (not sk or now_s - sk[1] / 1e9 > LIVE_WINDOW_S):
                a['state'] = 'idle'
            a['result_preview'] = result_preview(a.pop('result', None), 600 if detail else 240)
            alist.append(a)
        title_order = [p['title'] for p in meta['phases']]
        out = {'runId': run_id, 'session': os.path.basename(sess)[:8], 'session_title': self._session_title(sess),
               'name': meta['name'], 'description': meta['description'], 'tokens': None, 'tool_calls': None}
        if final:
            out['name'] = final.get('workflowName') or out['name']
            out['description'] = out['description'] or final.get('summary')
            out['status'] = final.get('status') or 'completed'
            out['started'] = final.get('startTime')
            out['duration_ms'] = final.get('durationMs')
            out['ended'] = utc_iso_ms(final.get('timestamp')) or (
                (out['started'] or 0) + (out['duration_ms'] or 0) if out['started'] else None)
            out['tokens'] = final.get('totalTokens')
            out['tool_calls'] = final.get('totalToolCalls')
            out['error'] = clip(final.get('error') or '', 400) if isinstance(final.get('error'), str) else None
            if not title_order:
                title_order = [p.get('title') for p in final.get('phases') or [] if isinstance(p, dict)]
            prog = {}
            for w in final.get('workflowProgress') or []:
                if isinstance(w, dict) and w.get('type') == 'workflow_agent' and w.get('agentId'):
                    prog[w['agentId']] = w
            known = {a['agentId'] for a in alist}
            for aid, w in prog.items():
                if aid not in known:
                    alist.append({'agentId': aid, 'label': w.get('label'), 'phase': w.get('phaseTitle'),
                                  'state': 'done', 'seq': len(alist), 'started': None, 'last_activity': None,
                                  'result_preview': clip(w.get('resultPreview') or '', 240)})
            for a in alist:
                w = prog.get(a['agentId'])
                if w:
                    a['tokens'] = w.get('tokens')
                    a['tool_calls'] = w.get('toolCalls')
                    a['duration_ms'] = w.get('durationMs')
                    a['started'] = a.get('started') or w.get('startedAt')
                    a['cached'] = bool(w.get('cached'))
                    if w.get('state') == 'progress' and a['state'] != 'done':
                        a['state'] = 'aborted'
                    if not a.get('result_preview') and w.get('resultPreview'):
                        a['result_preview'] = clip(w['resultPreview'], 600 if detail else 240)
                elif a['state'] in ('running', 'idle'):
                    a['state'] = 'aborted' if out['status'] == 'killed' else 'superseded'
        else:
            out['status'] = 'running' if now_s - latest_write < LIVE_WINDOW_S else 'stalled'
            starts = [a['started'] for a in alist if a.get('started')]
            out['started'] = min(starts) if starts else (int(jk[1] / 1e6) if jk else None)
            out['ended'] = None
            out['duration_ms'] = int(now_s * 1000 - out['started']) if out['started'] else None
            if out['status'] == 'stalled':
                for a in alist:
                    if a['state'] == 'running':
                        a['state'] = 'idle'
        for a in alist:
            if a.get('duration_ms') is None and a.get('started'):
                end = a['last_activity'] if a['state'] != 'running' else int(now_s * 1000)
                a['duration_ms'] = max(0, (end or a['started']) - a['started'])
        phases, pct = phase_progress(title_order, alist)
        if out['status'] == 'completed':
            pct = 100.0
            for p in phases:
                if p['started']:
                    p['state'] = 'done'
        details = {p['title']: p['detail'] for p in meta['phases']}
        for p in phases:
            p['detail'] = details.get(p['title'], '')
        out['phases'] = phases
        out['pct_approx'] = pct
        counted = [a for a in alist if a['state'] != 'superseded']
        out['agents_count'] = {
            'running': sum(1 for a in counted if a['state'] == 'running'),
            'done': sum(1 for a in counted if a['state'] == 'done'),
            'idle': sum(1 for a in counted if a['state'] == 'idle'),
            'aborted': sum(1 for a in counted if a['state'] == 'aborted'),
            'superseded': sum(1 for a in alist if a['state'] == 'superseded'),
            'total': len(counted)}
        done_with_result = [a for a in sorted(counted, key=lambda a: a.get('seq', 0)) if a['state'] == 'done'
                            and a.get('result_preview')]
        out['latest_results'] = [{'label': a['label'], 'text': (a['result_preview'] or '')[:240]}
                                 for a in done_with_result[-3:]][::-1]
        out['running_labels'] = [a['label'] for a in counted if a['state'] == 'running']
        if detail:
            for a in alist:
                if a['state'] == 'running':
                    a['current_tool'] = self.current_tool(sess, run_id, a['agentId'])
            out['agents'] = alist
        return out

    def list(self, limit=30):
        briefs = []
        for sess, r in self.runs():
            try:
                briefs.append(self.brief(sess, r))
            except (OSError, ValueError, KeyError, TypeError):
                continue
        rank = {'running': 0, 'stalled': 1}
        briefs.sort(key=lambda b: (rank.get(b['status'], 2), -(b.get('started') or 0)))
        return briefs[:limit]

    def detail(self, run_id):
        sess = self.find(run_id)
        return self.brief(sess, run_id, detail=True) if sess else None

    def _agent_records(self, sess, run_id, agent_id):
        if not AGENTID.match(agent_id or ''):
            return None
        p = os.path.join(sess, 'subagents', 'workflows', run_id, 'agent-%s.jsonl' % agent_id)
        if not os.path.isfile(p):
            return None
        return read_jsonl(p, AGENT_TAIL)

    def current_tool(self, sess, run_id, agent_id):
        recs = self._agent_records(sess, run_id, agent_id) or []
        for r in reversed(recs):
            if r.get('type') != 'assistant':
                continue
            content = (r.get('message') or {}).get('content') or []
            for c in reversed(content if isinstance(content, list) else []):
                if isinstance(c, dict) and c.get('type') == 'tool_use':
                    return {'name': c.get('name'), 'summary': tool_summary(c.get('name'), c.get('input')),
                            't': utc_iso_ms(r.get('timestamp'))}
        return None

    def agent_events(self, run_id, agent_id, tail=60):
        sess = self.find(run_id)
        if not sess:
            return None
        recs = self._agent_records(sess, run_id, agent_id)
        if recs is None:
            return None
        ev = []
        for r in recs:
            t = utc_iso_ms(r.get('timestamp'))
            if r.get('type') == 'assistant':
                for c in (r.get('message') or {}).get('content') or []:
                    if not isinstance(c, dict):
                        continue
                    if c.get('type') == 'tool_use':
                        ev.append({'t': t, 'kind': 'tool', 'name': c.get('name'),
                                   'summary': tool_summary(c.get('name'), c.get('input'))})
                    elif c.get('type') == 'text' and (c.get('text') or '').strip():
                        ev.append({'t': t, 'kind': 'text', 'name': '', 'summary': clip(c['text'].strip(), 400)})
            elif r.get('type') == 'user':
                content = (r.get('message') or {}).get('content')
                if isinstance(content, list):
                    for c in content:
                        if isinstance(c, dict) and c.get('type') == 'tool_result' and c.get('is_error'):
                            txt = c.get('content')
                            if isinstance(txt, list):
                                txt = ' '.join(x.get('text', '') for x in txt if isinstance(x, dict))
                            ev.append({'t': t, 'kind': 'error', 'name': '', 'summary': clip(txt, 300)})
        return {'events': ev[-tail:]}

    def plain_subagents_running(self):
        """Non-workflow subagents (Task tool) of this repo's sessions that wrote in the last 15 minutes."""
        out = []
        now_s = self.now()
        for sess in self._sessions():
            d = os.path.join(sess, 'subagents')
            try:
                names = os.listdir(d)
            except OSError:
                continue
            for f in names:
                if not (f.startswith('agent-') and f.endswith('.jsonl')):
                    continue
                sk = stat_key(os.path.join(d, f))
                if not sk or now_s - sk[1] / 1e9 > LIVE_WINDOW_S:
                    continue
                meta = read_json(os.path.join(d, f[:-6] + '.meta.json')) or {}
                out.append({'agentId': f[6:-6], 'label': meta.get('description') or f[6:-6],
                            'type': meta.get('agentType'), 'last_activity': int(sk[1] / 1e6),
                            'started': self._agent_start(os.path.join(d, f))})
        return out
