"""devdash unit tests: python -B -m unittest discover -s tools/devdash -p "test_*.py"

Fixtures in testdata/ are synthetic (no personal data, no game assets). Every test works on a
temporary copy, so mtimes and appended lines never touch the repo. Nothing here starts a game.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import dd_flows  # noqa: E402
import dd_runs  # noqa: E402
import dd_tasks  # noqa: E402
import dd_util  # noqa: E402
import devdash  # noqa: E402
from dd_proc import FakeProcs  # noqa: E402

FAKE_REPO = 'C:\\fake\\repo'


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='devdash-test-')
        shutil.copytree(os.path.join(HERE, 'testdata'), os.path.join(self.tmp, 'td'))
        self.td = os.path.join(self.tmp, 'td')
        self.runs_root = os.path.join(self.td, 'runs')
        self.state = os.path.join(self.td, 'state')
        self.projects = os.path.join(self.td, 'claude')
        # the batch fixture names its run by path: point it at this copy
        p = os.path.join(self.state, 'status', 'demo-batch.jsonl')
        with open(p, encoding='utf-8') as f:
            t = f.read()
        with open(p, 'w', encoding='utf-8', newline='') as f:
            f.write(t.replace('RUNS/', json.dumps(self.runs_root + os.sep)[1:-1]))
        self.now = time.time()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def store(self, procs=None, now=None):
        return dd_runs.RunStore(None, self.state, procs or FakeProcs(), extra_roots=[self.runs_root],
                                now=now or (lambda: self.now))

    def rk(self):
        return dd_runs.rootkey(self.runs_root)


class TestParse(Fixture):
    def read(self, name):
        with open(os.path.join(self.runs_root, name, 'supervisor.log'), 'rb') as f:
            return f.read().decode('utf-8')

    def test_pass_log(self):
        segs = dd_runs.parse_log(self.read('20261121-101500-demo-pass'))
        self.assertEqual(len(segs), 1)
        run = segs[0]
        self.assertEqual(run['pid'], 1111)
        self.assertEqual(run['plan']['steps_total'], 5)
        self.assertEqual(run['plan']['role'], 'host')
        self.assertTrue(run['plan']['game_dir'].endswith('DEATH STRANDING DIRECTORS CUT'))
        self.assertEqual([s['line'] for s in run['steps']], [5, 6, 7, 8, 9])
        # the until regex holds ' -> ': the step's text must not stop there
        self.assertEqual(run['steps'][2]['what'], 'until \\[sync\\] a -> b => pad A 120')
        self.assertEqual(run['steps'][2]['seconds'], 3.1)
        self.assertTrue(run['steps'][4]['optional'])
        self.assertEqual(run['steps'][4]['what'], '@guest screenshot S0-guest')
        self.assertEqual(run['final'], 'PASSED')
        self.assertEqual(run['exit'], 0)
        self.assertEqual(run['ds_pids'], [2001])
        self.assertEqual(run['phase'], 'done')
        self.assertIn('booting', run['phase_times'])
        # a line that is not a record continues the previous one
        trace = [m for _t, m in run['records'] if m.startswith('exit trace')][0]
        self.assertIn('\nsecond line of the exit trace', trace)

    def test_crlf_fail_and_refused(self):
        run = dd_runs.parse_log(self.read('20261121-102000-demo-fail'))[0]
        self.assertEqual(run['final'], 'FAILED')
        self.assertEqual([s['ok'] for s in run['steps']], [True, False])
        self.assertEqual(run['steps'][1]['what'], 'waitlog 30 \\[link\\] CONNECTED')
        self.assertEqual(run['stop'], 'a required step failed')
        ref = dd_runs.parse_log(self.read('20261121-103000-demo-refused'))[0]
        self.assertEqual(ref['final'], 'REFUSED')
        self.assertIn('timeout', ref['refused'])

    def test_restore_segment(self):
        segs = dd_runs.parse_log(self.read('20261121-105000-demo-repaired'))
        self.assertEqual([s['kind'] for s in segs], ['run', 'restore'])
        self.assertEqual(segs[0]['final'], 'FAILED')


class TestClassify(Fixture):
    def briefs(self, procs=None, now=None):
        st = self.store(procs, now)
        return {b['name']: b for b in st.list_runs(limit=100)['runs']}, st

    def test_states(self):
        b, _ = self.briefs()
        self.assertEqual(b['20261121-101500-demo-pass']['state'], 'passed')
        self.assertTrue(b['20261121-101500-demo-pass']['ok'])
        self.assertTrue(b['20261121-101500-demo-pass']['two_games'])
        self.assertEqual(b['20261121-101500-demo-pass']['shots'], 2)
        self.assertEqual(b['20261121-101500-demo-pass']['pct'], 100.0)
        self.assertEqual(b['20261121-102000-demo-fail']['state'], 'failed')
        self.assertEqual(b['20261121-102000-demo-fail']['failed_lines'], [4])
        self.assertEqual(b['20261121-103000-demo-refused']['state'], 'refused')
        self.assertEqual(b['20261121-104000-demo-interrupted']['state'], 'interrupted')
        self.assertTrue(b['20261121-104000-demo-interrupted']['needs_restore'])
        self.assertIn('Restore-TestRun.ps1', b['20261121-104000-demo-interrupted']['restore_cmd'])
        self.assertFalse(b['20261121-101500-demo-pass']['needs_restore'])
        # summary says pass but the terminal line says FAILED: the log line is the verdict
        rep = b['20261121-105000-demo-repaired']
        self.assertEqual(rep['state'], 'failed')
        self.assertEqual(rep['verdict']['result'], 'pass')
        self.assertIn('game folder not restored', rep['verdict']['issues'])
        self.assertTrue(rep['repaired_later'])
        self.assertFalse(rep['ok'])
        later, _ = self.briefs(now=lambda: time.time() + 600)  # an old folder without a supervisor.log
        self.assertEqual(later['probe-1']['state'], 'aborted-before-start')

    def test_live_by_supervisor_pid(self):
        procs = FakeProcs([(4242, 1, 'st_testenv_supervisor.exe')], [{'pid': 4242, 'run_dir': None, 'mode': 'run'}])
        b, st = self.briefs(procs, now=lambda: dd_util.local_iso_ms('2026-11-21T10:40:30.000') / 1000.0)
        live = b['20261121-104000-demo-interrupted']
        self.assertEqual(live['state'], 'live')
        self.assertFalse(live['needs_restore'])
        self.assertEqual(live['steps_done'], 1)
        self.assertEqual(live['pct'], 25.0)
        self.assertAlmostEqual(live['elapsed_s'], 30.0, delta=1.0)
        self.assertTrue(live['game_live'])  # phase 'steps': the game folder may be read
        # from 'finishing' on the supervisor collects, deletes and restores the game folder: devdash reads the run folder
        sl = os.path.join(self.runs_root, '20261121-104000-demo-interrupted', 'supervisor.log')
        with open(sl, 'ab') as f:
            f.write(b'[   31.000] scenario completed\r\n[   32.000] ds.exe exit code 0\r\n[   33.000] restore: removed x\r\n')
        b4, st4 = self.briefs(procs, now=lambda: dd_util.local_iso_ms('2026-11-21T10:40:30.000') / 1000.0)
        r4 = b4['20261121-104000-demo-interrupted']
        self.assertEqual((r4['state'], r4['phase'], r4['game_live']), ('live', 'restoring', False))
        self.assertEqual(st4._log_path(self.rk(), r4['name'], os.path.dirname(sl), r4, 'host')[1], 'run')
        with open(sl, 'rb') as f:
            t = f.read()
        with open(sl, 'wb') as f:
            f.write(t[:t.index(b'[   31.000]')])
        lv = st.live_runs(procs.supervisors())
        self.assertEqual([r['name'] for r in lv], ['20261121-104000-demo-interrupted'])
        # the next step comes from scenario.txt (step lines are its line numbers)
        self.assertEqual(lv[0]['next_step'], {'line': 4, 'what': 'pad A 150'})
        self.assertIn('pad A 150', lv[0]['current'])
        # a held session (supervisor --hold) says so in progress.jsonl
        with open(os.path.join(self.runs_root, '20261121-104000-demo-interrupted', 'progress.jsonl'), 'w', encoding='utf-8') as f:
            f.write('{"t":1.0,"event":"plan","total":4}\n{"t":9.0,"event":"hold","state":"ready"}\n')
        b3, _ = self.briefs(procs, now=lambda: dd_util.local_iso_ms('2026-11-21T10:40:30.000') / 1000.0)
        self.assertEqual(b3['20261121-104000-demo-interrupted']['phase'], 'held session')
        # a pid reused by another program is not a live supervisor
        b2, _ = self.briefs(FakeProcs([(4242, 1, 'notepad.exe')]))
        self.assertEqual(b2['20261121-104000-demo-interrupted']['state'], 'interrupted')

    def test_starting_and_aborted(self):
        os.makedirs(os.path.join(self.runs_root, '20261121-110000-fresh'))
        b, _ = self.briefs(now=lambda: time.time())
        self.assertEqual(b['20261121-110000-fresh']['state'], 'starting')
        b, _ = self.briefs(now=lambda: time.time() + 600)
        self.assertEqual(b['20261121-110000-fresh']['state'], 'aborted-before-start')

    def test_filters_and_paging(self):
        st = self.store()
        r = st.list_runs(limit=2)
        self.assertEqual([x['name'] for x in r['runs']], ['20261121-105000-demo-repaired', '20261121-104000-demo-interrupted'])
        self.assertEqual(r['next'], self.rk() + '/20261121-104000-demo-interrupted')
        r2 = st.list_runs(limit=2, before=r['next'])
        self.assertEqual(r2['runs'][0]['name'], '20261121-103000-demo-refused')
        self.assertEqual({x['name'] for x in st.list_runs(state='passed')['runs']}, {'20261121-101500-demo-pass'})
        failed = {x['name'] for x in st.list_runs(state='failed')['runs']}
        self.assertIn('20261121-104000-demo-interrupted', failed)
        self.assertIn('20261121-102000-demo-fail', failed)
        self.assertEqual([x['name'] for x in st.list_runs(scenario='REFUSED')['runs']], ['20261121-103000-demo-refused'])

    def test_resolve_is_path_safe(self):
        st = self.store()
        rk = self.rk()
        self.assertTrue(st.resolve(rk, '20261121-101500-demo-pass'))
        for bad in ('..', '..\\runs', '../x', 'C:\\Windows', '', '.', 'a/b', '20261121-101500-demo-pass\\..'):
            self.assertIsNone(st.resolve(rk, bad), bad)
        self.assertIsNone(st.resolve('0000000000', '20261121-101500-demo-pass'))
        self.assertIsNone(st.resolve('../../etc', '20261121-101500-demo-pass'))
        self.assertEqual(st.shot_bytes(rk, '20261121-101500-demo-pass', 'host', '..\\summary.json')[0], None)
        self.assertEqual(st.shot_bytes(rk, '20261121-101500-demo-pass', 'other', 'S0-host.png')[0], None)
        data, final = st.shot_bytes(rk, '20261121-101500-demo-pass', 'guest', 'S0-guest.png')
        self.assertTrue(data.startswith(b'\x89PNG'))
        self.assertTrue(final)

    def test_detail_logs_and_shots(self):
        st = self.store()
        d = st.detail(self.rk(), '20261121-101500-demo-pass')
        self.assertEqual(len(d['steps']), 5)
        self.assertEqual([(s['side'], s['name']) for s in d['shots_list']], [('host', 'S0-host.png'), ('guest', 'S0-guest.png')])
        self.assertTrue(d['files']['host'] and d['files']['guest'] and d['files']['supervisor'])
        self.assertFalse(d['files']['host_stdout'])
        self.assertNotIn('game_dir', d['plan'])
        r = st.log_lines(self.rk(), '20261121-101500-demo-pass', 'host', None, 10)
        self.assertEqual(len(r['lines']), 10)
        self.assertEqual(r['lines'][-1], '[  19.500][100] [demo] host line 39')
        self.assertTrue(r['truncated_head'])
        self.assertEqual(r['source'], 'run')
        r2 = st.log_lines(self.rk(), '20261121-101500-demo-pass', 'host', r['offset'], 10)
        self.assertEqual(r2['lines'], [])
        self.assertFalse(r2['reset'])
        self.assertTrue(st.log_lines(self.rk(), '20261121-101500-demo-pass', 'nope')['missing'])

    def test_counts_today(self):
        before = self.store().counts_today()
        name = time.strftime('%Y%m%d') + '-000001-today-pass'
        shutil.copytree(os.path.join(self.runs_root, '20261121-101500-demo-pass'), os.path.join(self.runs_root, name))
        c = self.store().counts_today()
        self.assertEqual(c['passed'], before['passed'] + 1)
        self.assertEqual(c['total'], before['total'] + 1)


class TestUtil(unittest.TestCase):
    def test_incremental_reader(self):
        d = tempfile.mkdtemp()
        try:
            p = os.path.join(d, 'x.log')
            with open(p, 'wb') as f:
                f.write(b'one\r\ntwo\nthr')
            r = dd_util.read_lines_from(p)
            self.assertEqual(r['lines'], ['one', 'two'])  # the partial third line waits
            self.assertEqual(r['offset'], 9)
            with open(p, 'ab') as f:
                f.write(b'ee\nfour\n')
            r2 = dd_util.read_lines_from(p, r['offset'])
            self.assertEqual(r2['lines'], ['three', 'four'])
            self.assertFalse(r2['reset'])
            with open(p, 'wb') as f:  # the mod log is reopened "w" when a game starts
                f.write(b'new\n')
            r3 = dd_util.read_lines_from(p, r2['offset'])
            self.assertTrue(r3['reset'])
            self.assertEqual(r3['lines'], ['new'])
            big = b''.join(b'line %d\n' % i for i in range(5000))
            with open(p, 'wb') as f:
                f.write(big)
            r4 = dd_util.read_lines_from(p, None, tail_lines=50, max_bytes=4096)
            self.assertEqual(len(r4['lines']), 50)
            self.assertEqual(r4['lines'][-1], 'line 4999')
            self.assertTrue(r4['truncated_head'])
            self.assertIsNone(dd_util.read_lines_from(os.path.join(d, 'missing.log')))
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_redaction(self):
        r = dd_util.Redactor(home='C:\\Users\\someone')
        s = ('saves C:\\Users\\someone\\AppData\\Local\\KojimaProductions\\DeathStrandingDC\\12345678\\profile '
             'and c:/users/someone/x and C:\\\\Users\\\\someone\\\\y epic_0123456789abcdef 76561190000000001 '
             'Steam\\userdata\\12345\\x')
        out = r.text(s)
        self.assertNotIn('someone', out)
        self.assertNotIn('12345678', out)
        self.assertNotIn('0123456789abcdef', out)
        self.assertNotIn('76561190000000001', out)
        self.assertNotIn('12345', out)
        self.assertIn('%USERPROFILE%\\AppData', out)
        self.assertIn('DeathStrandingDC\\<account>', out)
        self.assertEqual(r.obj({'k': ['C:\\Users\\someone\\a']}), {'k': ['%USERPROFILE%\\a']})
        # Git Bash (agents' Bash commands), WSL and another drive's Users folder
        for s in ('cat /c/Users/someone/AppData/x', 'ls /C/users/Someone/y', '/mnt/c/Users/someone/z', 'D:\\Users\\someone\\q'):
            self.assertNotIn('someone', r.text(s).lower(), s)
        self.assertEqual(r.text('cat /c/Users/someone/AppData/x'), 'cat %USERPROFILE%/AppData/x')
        self.assertEqual(r.text('C:\\Users\\Public\\x'), 'C:\\Users\\Public\\x')
        # clip redacts before it cuts: no half user path survives
        self.assertNotIn('C:\\Users\\', dd_util.clip(dd_util.REDACT.text(os.path.expanduser('~')) + 'x', 10)
                         if os.path.expanduser('~').startswith('C:\\Users\\') else '')

    def test_times(self):
        self.assertEqual(dd_util.utc_iso_ms('1970-01-01T00:00:01.500Z'), 1500)
        self.assertIsNone(dd_util.utc_iso_ms('nope'))
        self.assertIsNotNone(dd_util.local_stamp_ms('20261121-075508'))
        self.assertIsNone(dd_util.local_stamp_ms('probe'))
        a = dd_util.local_iso_ms('2026-11-21T07:55:08.434')
        self.assertEqual(a - dd_util.local_stamp_ms('20261121-075508'), 434)


class TestFlows(Fixture):
    def flows(self, now):
        return dd_flows.FlowStore(FAKE_REPO, self.projects, now=lambda: now)

    def touch_all(self, t):
        for dp, _dn, fn in os.walk(self.projects):
            for f in fn:
                os.utime(os.path.join(dp, f), (t, t))

    def test_meta(self):
        with open(os.path.join(self.projects, 'C--fake-repo', '11111111-2222-3333-4444-555555555555', 'workflows', 'scripts',
                               'demo-research-wf_test0001-aaa.js'), encoding='utf-8') as f:
            m = dd_flows.parse_meta(f.read())
        self.assertEqual(m['name'], 'demo-research')
        self.assertEqual([p['title'] for p in m['phases']], ['Research', 'Implement', 'Review'])
        self.assertEqual(m['phases'][2]['detail'], 'check it')

    def test_slug_and_runs(self):
        self.assertEqual(dd_flows.slug_of('C:\\Projects\\stranded-together'), 'C--Projects-stranded-together')
        fl = self.flows(time.time())
        self.assertEqual(sorted(r for _s, r in fl.runs()), ['wf_test0001-aaa', 'wf_test0002-bbb'])
        self.assertIsNone(fl.find('wf_nope-000'))
        self.assertIsNone(fl.find('../x'))

    def test_live_workflow(self):
        t = time.time()
        self.touch_all(t - 60)
        fl = self.flows(t)
        w = fl.detail('wf_test0001-aaa')
        self.assertEqual(w['status'], 'running')
        self.assertEqual(w['name'], 'demo-research')
        self.assertEqual(w['session_title'], 'Demo session')
        st = {a['agentId']: a['state'] for a in w['agents']}
        self.assertEqual(st['a0000000000000002'], 'superseded')  # retried under the same label
        self.assertEqual(st['a0000000000000001'], 'done')
        self.assertEqual(st['a0000000000000004'], 'running')
        self.assertEqual(w['agents_count'], {'running': 1, 'done': 2, 'idle': 0, 'aborted': 0, 'superseded': 1, 'total': 3})
        ph = {p['title']: p for p in w['phases']}
        self.assertEqual((ph['Research']['state'], ph['Research']['started'], ph['Research']['done']), ('done', 2, 2))
        self.assertEqual(ph['Implement']['state'], 'active')
        self.assertEqual(ph['Review']['state'], 'pending')
        self.assertAlmostEqual(w['pct_approx'], 33.3, delta=0.1)
        cur = [a for a in w['agents'] if a['agentId'] == 'a0000000000000004'][0]['current_tool']
        self.assertEqual(cur['name'], 'Bash')
        self.assertEqual(cur['summary'], 'Build the mod')
        self.assertEqual(w['started'], dd_util.utc_iso_ms('2026-11-21T10:00:00.000Z'))
        ev = fl.agent_events('wf_test0001-aaa', 'a0000000000000004')['events']
        self.assertEqual([e['kind'] for e in ev], ['tool', 'error'])
        self.assertIsNone(fl.agent_events('wf_test0001-aaa', '../../x'))
        self.assertEqual(w['latest_results'][0]['label'], 'research:beta')
        # stalled: no write for 15 minutes and no final json
        w2 = self.flows(t + 3600).brief(fl.find('wf_test0001-aaa'), 'wf_test0001-aaa')
        self.assertEqual(w2['status'], 'stalled')
        self.assertEqual(w2['agents_count']['running'], 0)

    def test_finished_workflow(self):
        t = time.time()
        self.touch_all(t - 60)  # the fixture's own mtimes would make the running one stalled after 15 minutes
        fl = self.flows(t)
        w = fl.detail('wf_test0002-bbb')
        self.assertEqual(w['status'], 'completed')
        self.assertEqual(w['name'], 'demo-done')
        self.assertEqual(w['pct_approx'], 100.0)
        self.assertEqual(w['tokens'], 12345)
        self.assertEqual(w['duration_ms'], 3600000)
        self.assertEqual([p['state'] for p in w['phases']], ['done', 'done'])
        self.assertEqual({a['label']: a['tokens'] for a in w['agents']}, {'build': 100, 'review': 200})
        # running first, then stalled, then finished ones
        self.assertEqual([(b['runId'], b['status']) for b in fl.list()],
                         [('wf_test0001-aaa', 'running'), ('wf_test0002-bbb', 'completed')])
        self.assertEqual([(b['runId'], b['status']) for b in self.flows(t + 3600).list()],
                         [('wf_test0001-aaa', 'stalled'), ('wf_test0002-bbb', 'completed')])


class TestTasks(Fixture):
    def tasks(self, procs=None, now=None):
        return dd_tasks.TaskStore(self.state, procs or FakeProcs(), now=now or (lambda: dd_util.utc_iso_ms('2026-11-21T10:25:00.000Z') / 1000.0))

    def test_fold(self):
        ts = self.tasks()
        ids = ts.ids()
        self.assertEqual(ids, ['demo-batch', 'demo-research'])  # 'Bad Name' is not a task id
        b = ts.detail('demo-batch')
        self.assertEqual(b['kind'], 'batch')
        self.assertEqual(b['status'], 'running')
        self.assertEqual(b['total'], 3)
        self.assertEqual(b['done'], 1)
        self.assertEqual(b['items_count']['pass'], 1)
        self.assertEqual(b['items_count']['running'], 1)  # the partial last line is not applied yet
        self.assertEqual(b['current'], '02-demo-fail')
        self.assertEqual(b['log'][0]['msg'], 'demo-fail is slow')
        r = ts.detail('demo-research')
        self.assertEqual((r['status'], r['done'], r['total'], r['pct']), ('pass', 4, 4, 100.0))  # nothing after close
        self.assertIsNone(ts.detail('../demo-batch'))

    def test_append_and_stale(self):
        ts = self.tasks()
        with open(os.path.join(self.state, 'status', 'demo-batch.jsonl'), 'ab') as f:
            f.write(b'tus":"fail"}\n')
        b = ts.detail('demo-batch')
        self.assertEqual(b['items_count']['fail'], 1)
        self.assertEqual(b['done'], 2)
        # silent for longer than max(3 x heartbeat, 900 s): stale
        late = self.tasks(now=lambda: dd_util.utc_iso_ms('2026-11-21T11:30:00.000Z') / 1000.0)
        self.assertEqual(late.detail('demo-batch')['status'], 'stale')

    def test_briefs_are_copies_and_stale_ages_out(self):
        ts = self.tasks()
        b = ts.detail('demo-batch')
        b['items'][0]['run'] = {'state': 'live'}
        b['log'].append({'msg': 'x'})
        again = ts.detail('demo-batch')
        self.assertNotIn('run', again['items'][0])  # the cache's own dicts were not changed
        self.assertNotEqual(again['log'][-1].get('msg'), 'x')
        # concurrent folds of a growing file apply every record once
        p = os.path.join(self.state, 'status', 'demo-batch.jsonl')
        with open(p, 'ab') as f:
            f.write(b'tus":"fail"}\n')
        n0 = len(ts.detail('demo-batch')['log'])
        for k in range(20):
            dd_tasks.post(self.state, 'demo-batch', [{'op': 'log', 'msg': 'line %d' % k}])
        th = [threading.Thread(target=ts.list, kwargs={'with_items': True}) for _ in range(6)]
        th += [threading.Thread(target=ts.detail, args=('demo-batch',)) for _ in range(6)]
        for t in th:
            t.start()
        for t in th:
            t.join()
        msgs = [x['msg'] for x in ts.detail('demo-batch')['log']]
        self.assertEqual(len(msgs), n0 + 20)
        self.assertEqual(len(set(m for m in msgs if m.startswith('line '))), 20)
        # a stale batch (abandoned) leaves the overview after a day without a record; ?all=1 keeps it
        day_later = self.tasks(now=lambda: dd_util.utc_iso_ms('2026-11-23T10:25:00.000Z') / 1000.0)
        self.assertNotIn('demo-batch', [t['id'] for t in day_later.list()])
        self.assertIn('demo-batch', [t['id'] for t in day_later.list(include_old=True)])

    def test_post_and_reopen(self):
        dd_tasks.post(self.state, 'mine', [{'op': 'open', 'kind': 'test', 'title': 'Mine', 'total': 2, 'pid': 99999}])
        dd_tasks.post(self.state, 'mine', [{'op': 'progress', 'done': 1}, {'op': 'log', 'msg': 'x' * 20000}])
        ts = dd_tasks.TaskStore(self.state, FakeProcs([(99999, 1, 'pwsh.exe')]))
        m = ts.detail('mine')
        self.assertEqual((m['status'], m['done'], m['pct']), ('running', 1, 50.0))
        self.assertLessEqual(len(m['log'][-1]['msg']), 2000)
        self.assertEqual(dd_tasks.TaskStore(self.state, FakeProcs()).detail('mine')['status'], 'stale')  # its pid is gone
        dd_tasks.post(self.state, 'mine', [{'op': 'close', 'status': 'fail'}, {'op': 'open', 'kind': 'test', 'title': 'Again'}])
        m = ts.detail('mine')
        self.assertEqual((m['status'], m['title'], m['done']), ('running', 'Again', None))
        with self.assertRaises(ValueError):
            dd_tasks.post(self.state, '../evil', [{'op': 'beat'}])

    def test_cli_post(self):
        rc = devdash.main(['--state-dir', self.state, 'post', 'cli-task', '--title', 'CLI', '--kind', 'build', '--total', '3',
                           '--done', '2', '--log', 'hello', '--item', 'a', '--item-status', 'pass'])
        self.assertEqual(rc, 0)
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('cli-task')
        self.assertEqual((t['kind'], t['title'], t['done'], t['total']), ('build', 'CLI', 2, 3))
        devdash.main(['--state-dir', self.state, 'post', 'cli-task', '--close', 'done', '--summary', 'ok'])
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('cli-task')
        self.assertEqual((t['status'], t['summary']), ('done', 'ok'))


class TestCtest(Fixture):
    def test_running_ctest_reads_the_tmp_log(self):
        repo = os.path.join(self.tmp, 'repo')
        tmpd = os.path.join(repo, 'build', 'Testing', 'Temporary')
        os.makedirs(tmpd)
        with open(os.path.join(tmpd, 'LastTest.log'), 'w', encoding='utf-8') as f:
            f.write('1/2 Testing: a\n2/2 Testing: b\nEnd testing: Nov 21 10:00 CST\n')
        old = time.time() - 60
        os.utime(os.path.join(tmpd, 'LastTest.log'), (old, old))
        with open(os.path.join(tmpd, 'LastTest.log.tmp1a2b3'), 'w', encoding='utf-8') as f:
            f.write('1/3 Testing: one\n2/3 Testing: two\n')
        d = devdash.Dash(repo, self.state, self.projects, FakeProcs([(77, 1, 'ctest.exe')]))
        c = d.ctest()
        self.assertEqual(len(c), 1)
        self.assertEqual((c[0]['state'], c[0]['k'], c[0]['n'], c[0]['current']), ('running', 1, 3, 'two'))
        os.remove(os.path.join(tmpd, 'LastTest.log.tmp1a2b3'))  # ctest renamed it at the end
        c = devdash.Dash(repo, self.state, self.projects, FakeProcs()).ctest()
        self.assertEqual((c[0]['state'], c[0]['k'], c[0]['n']), ('done', 2, 2))


class TestServer(Fixture):
    def setUp(self):
        super().setUp()
        dash = devdash.Dash(repo=self.td, state_dir=self.state, projects_dir=self.projects, procs=FakeProcs(),
                            extra_roots=[self.runs_root])
        dash.flows.repo = FAKE_REPO
        self.srv = devdash.Server(('127.0.0.1', 0), devdash.Handler)
        devdash.Handler.dash = dash
        devdash.Handler.port = self.srv.server_address[1]
        devdash.Handler.log_path = None
        self.base = 'http://127.0.0.1:%d' % self.srv.server_address[1]
        self.th = threading.Thread(target=self.srv.serve_forever, kwargs={'poll_interval': 0.1}, daemon=True)
        self.th.start()

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()
        super().tearDown()

    def get(self, path, headers=None, method='GET'):
        req = urllib.request.Request(self.base + path, headers=headers or {}, method=method)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, r.read(), dict(r.headers)
        except urllib.error.HTTPError as e:
            return e.code, e.read(), dict(e.headers)

    def test_api(self):
        code, body, h = self.get('/api/ping')
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['app'], 'devdash')
        self.assertIn("default-src 'self'", h['Content-Security-Policy'])
        code, body, h = self.get('/api/overview')
        o = json.loads(body)
        self.assertEqual(code, 200)
        self.assertEqual([b['id'] for b in o['batches']], ['demo-batch'])
        item = o['batches'][0]['items'][0]
        self.assertEqual(item['run']['name'], '20261121-101500-demo-pass')  # linked through run_dir
        self.assertEqual(item['run']['state'], 'passed')
        code304, _b, _h = self.get('/api/overview', {'If-None-Match': h['ETag']})
        self.assertIn(code304, (200, 304))
        rk = dd_runs.rootkey(self.runs_root)
        code, body, _h = self.get('/api/runs?limit=3')
        self.assertEqual(len(json.loads(body)['runs']), 3)
        code, body, _h = self.get('/api/runs/%s/20261121-101500-demo-pass' % rk)
        self.assertEqual(json.loads(body)['state'], 'passed')
        code, body, h = self.get('/api/runs/%s/20261121-101500-demo-pass/shot/host/S0-host.png' % rk)
        self.assertEqual((code, h['Content-Type']), (200, 'image/png'))
        self.assertTrue(body.startswith(b'\x89PNG'))
        code, body, _h = self.get('/api/runs/%s/20261121-101500-demo-pass/log?which=supervisor&tail=5' % rk)
        self.assertEqual(len(json.loads(body)['lines']), 5)
        code, body, _h = self.get('/api/workflows')
        self.assertEqual(len(json.loads(body)['workflows']), 2)
        code, body, _h = self.get('/api/workflows/wf_test0001-aaa/agents/a0000000000000004')
        self.assertEqual(code, 200)
        code, body, _h = self.get('/api/tasks/demo-batch')
        self.assertEqual(json.loads(body)['id'], 'demo-batch')
        # the profile path never reaches the page
        self.assertNotIn(os.path.expanduser('~').replace('\\', '\\\\').encode(), self.get('/api/overview')[1])

    def test_refusals(self):
        rk = dd_runs.rootkey(self.runs_root)
        self.assertEqual(self.get('/api/ping', {'Host': 'evil.example:80'})[0], 403)
        self.assertEqual(self.get('/api/ping', method='POST')[0], 405)
        self.assertEqual(self.get('/api/ping', method='DELETE')[0], 405)
        for p in ('/../README.md', '/static/../devdash.py', '/devdash.py', '/api/runs/%s/..' % rk,
                  '/api/runs/%s/%%2e%%2e%%5csummary.json' % rk,
                  '/api/runs/%s/20261121-101500-demo-pass/shot/host/..%%5Csummary.json' % rk,
                  '/api/runs/%s/20261121-101500-demo-pass/shot/host/summary.json' % rk,
                  '/api/workflows/..%5C..', '/api/tasks/..%5Cx', '/api/nope'):
            self.assertEqual(self.get(p)[0], 404, p)
        self.assertEqual(self.get('/')[0], 200)
        self.assertEqual(self.get('/app.js')[0], 200)


def find_powershell():
    for exe in ('pwsh', 'powershell'):
        p = shutil.which(exe)
        if p:
            return p
    return None


@unittest.skipUnless(os.name == 'nt' and find_powershell(), 'needs Windows PowerShell or pwsh')
class TestScripts(Fixture):
    def ps(self, *args, env=None):
        e = dict(os.environ)
        e.update(env or {})
        return subprocess.run([find_powershell(), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File'] + list(args),
                              capture_output=True, text=True, timeout=120, env=e)

    def test_set_devstatus(self):
        s = os.path.join(HERE, 'Set-DevStatus.ps1')
        r = self.ps(s, '-Id', 'ps-task', '-Title', 'From PowerShell', '-Kind', 'research', '-Total', '4', '-StateDir', self.state)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.ps(s, '-Id', 'ps-task', '-Done', '2', '-Log', 'half way', '-Level', 'warn', '-StateDir', self.state)
        self.ps(s, '-Id', 'ps-task', '-Item', 'x', '-ItemStatus', 'pass', '-RunDir', 'C:\\nowhere\\runs\\r', '-StateDir', self.state)
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('ps-task')
        self.assertEqual((t['kind'], t['title'], t['total'], t['done']), ('research', 'From PowerShell', 4, 2))
        self.assertEqual(t['log'][-1]['level'], 'warn')
        self.assertEqual(t['items'][0]['status'], 'pass')
        self.ps(s, '-Id', 'ps-task', '-Status', 'done', '-Summary', 'finished', '-StateDir', self.state)
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('ps-task')
        self.assertEqual((t['status'], t['summary']), ('done', 'finished'))
        self.assertNotEqual(self.ps(s, '-Id', '../bad', '-StateDir', self.state).returncode, 0)
        # an id in upper case is lowered (devdash lists lower-case ids only); -Detail '' clears a row's detail
        self.assertEqual(self.ps(s, '-Id', 'PS-Upper', '-Title', 'U', '-Item', 'r', '-Detail', 'waiting', '-StateDir', self.state).returncode, 0)
        self.ps(s, '-Id', 'PS-Upper', '-Item', 'r', '-Detail', '', '-StateDir', self.state)
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('ps-upper')
        self.assertIsNotNone(t)
        self.assertEqual(t['items'][0]['detail'], '')

    def test_run_batch_with_a_fake_run_script(self):
        for exe in ('ds.exe', 'st_testenv_supervisor.exe'):
            if subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + exe, '/NH'], capture_output=True, text=True).stdout.count(exe):
                self.skipTest('a hidden run is active (%s): the batch would wait for it' % exe)
        rb = os.path.join(os.path.dirname(HERE), 'testenv', 'Run-Batch.ps1')
        build = os.path.join(self.tmp, 'bld')
        r = self.ps(rb, '-Scenarios', 'alpha,beta-fail,gamma', '-Id', 'test-batch', '-BuildDir', build, '-WaitForGameSec', '0',
                    '-RunScript', os.path.join(self.td, 'fake-start-testrun.ps1'), '-SeedSaves', 'complete-100',
                    env={'ST_DEVDASH_DIR': self.state})
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)  # one failed
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('test-batch')
        if 'still active after' in (t['summary'] or ''):
            self.skipTest('another hidden run started meanwhile')
        self.assertEqual(t['status'], 'fail')
        self.assertEqual(t['kind'], 'batch')
        self.assertEqual([(i['key'], i['status']) for i in t['items']],
                         [('01-alpha', 'pass'), ('02-beta-fail', 'fail'), ('03-gamma', 'pass')])
        self.assertTrue(all(i.get('run_dir') for i in t['items']))
        self.assertEqual(t['items'][1]['verdict']['result'], 'fail')
        self.assertEqual((t['done'], t['total']), (3, 3))
        self.assertIn('2 passed, 1 failed', t['summary'])
        # the first run builds, the rest pass -NoBuild; -SeedSaves reaches every run
        details = []
        for i in t['items']:
            with open(os.path.join(i['run_dir'], 'summary.json'), encoding='utf-8-sig') as f:
                steps = json.load(f)['steps']
            details.append((steps[0] if isinstance(steps, list) else steps)['detail'])
        self.assertTrue(details[0].startswith('nobuild=False seed=complete-100'), details)
        self.assertTrue(details[1].startswith('nobuild=True'), details)
        # -StopOnFail stops at the failure and skips the rest
        r = self.ps(rb, '-Scenarios', 'one-fail,two', '-Id', 'test-batch2', '-BuildDir', build, '-WaitForGameSec', '0',
                    '-RunScript', os.path.join(self.td, 'fake-start-testrun.ps1'), '-StopOnFail', env={'ST_DEVDASH_DIR': self.state})
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('test-batch2')
        self.assertEqual([i['status'] for i in t['items']], ['fail', 'skip'])
        # a batch in parts (-From k -Count n): one task, open between the parts, closed by the last with every item counted
        part = lambda k: self.ps(rb, '-Scenarios', 'alpha,beta-fail,gamma', '-Id', 'test-batch3', '-BuildDir', build,
                                 '-WaitForGameSec', '0', '-RunScript', os.path.join(self.td, 'fake-start-testrun.ps1'),
                                 '-From', str(k), '-Count', '1', env={'ST_DEVDASH_DIR': self.state})
        self.assertEqual(part(1).returncode, 0)
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('test-batch3')
        self.assertEqual((t['status'], t['done'], [i['status'] for i in t['items']]), ('running', 1, ['pass', 'queued', 'queued']))
        self.assertEqual(part(2).returncode, 1)
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('test-batch3')
        self.assertEqual((t['status'], t['done'], [i['status'] for i in t['items']]), ('running', 2, ['pass', 'fail', 'queued']))
        self.assertEqual(part(3).returncode, 1)  # the batch as a whole had a failure
        t = dd_tasks.TaskStore(self.state, FakeProcs()).detail('test-batch3')
        self.assertEqual((t['status'], t['done'], [i['status'] for i in t['items']]), ('fail', 3, ['pass', 'fail', 'pass']))
        self.assertIn('2 passed, 1 failed (3 of 3 run)', t['summary'])


if __name__ == '__main__':
    unittest.main()
