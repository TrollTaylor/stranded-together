'use strict';
/* devdash page: polls the local devdash server (read only) and renders without flicker:
   every section is patched in place, keyed by run / task / workflow id, and only when its HTML changed. */

const $ = (s, el = document) => el.querySelector(s);
const $$ = (s, el = document) => Array.from(el.querySelectorAll(s));
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const enc = encodeURIComponent;

function patch(el, html) {
  if (el && el._h !== html) { el.innerHTML = html; el._h = html; }
}

/* keyed list: one child element per item, created once, updated in place, kept in order */
function keyed(container, items, keyOf, render, emptyHtml, tag) {
  const old = new Map();
  for (const c of Array.from(container.children)) {
    if (c.dataset && c.dataset.key) old.set(c.dataset.key, c); else c.remove();
  }
  let prev = null;
  if (!items.length && emptyHtml) {
    items = [{ __empty: true }];
    keyOf = () => '__empty';
    const r0 = render;
    render = (it, el) => { if (it.__empty) { el.className = 'empty'; patch(el, emptyHtml); } else r0(it, el); };
  }
  for (const it of items) {
    const k = String(keyOf(it));
    let el = old.get(k);
    if (el) old.delete(k); else { el = document.createElement(tag || 'div'); el.dataset.key = k; }
    render(it, el);
    const want = prev ? prev.nextSibling : container.firstChild;
    if (el !== want) container.insertBefore(el, want);
    prev = el;
  }
  for (const el of old.values()) el.remove();
}

/* ---------- formatting */
const pad2 = (n) => String(n).padStart(2, '0');
function fmtDur(s) {
  if (s == null || isNaN(s)) return '';
  s = Math.max(0, Math.round(s));
  if (s < 60) return s + 's';
  const m = Math.floor(s / 60);
  if (m < 60) return m + 'm ' + pad2(s % 60) + 's';
  const h = Math.floor(m / 60);
  return h + 'h ' + pad2(m % 60) + 'm';
}
function fmtTime(ms) {
  if (!ms) return '';
  const d = new Date(ms);
  return pad2(d.getHours()) + ':' + pad2(d.getMinutes()) + ':' + pad2(d.getSeconds());
}
function fmtWhen(ms) {
  if (!ms) return '';
  const d = new Date(ms);
  const now = new Date();
  const t = pad2(d.getHours()) + ':' + pad2(d.getMinutes());
  if (d.toDateString() === now.toDateString()) return t;
  const y = new Date(now); y.setDate(now.getDate() - 1);
  if (d.toDateString() === y.toDateString()) return 'yesterday ' + t;
  return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' }) + ' ' + t;
}
function fmtAgo(ms) {
  if (!ms) return '';
  const s = (Date.now() - ms) / 1000;
  if (s < 5) return 'just now';
  if (s < 3600 * 6) return fmtDur(s) + ' ago';
  return fmtWhen(ms);
}
function fmtNum(n) {
  if (n == null) return '';
  if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
  if (n >= 1e4) return Math.round(n / 1e3) + 'k';
  return String(n);
}
const badge = (cls, text, pulse) => `<span class="badge ${esc(cls)}${pulse ? ' pulse' : ''}">${esc(text)}</span>`;
function bar(pct, cls) {
  const p = pct == null ? 0 : Math.max(0, Math.min(100, pct));
  return `<div class="bar ${cls || ''}"><i style="width:${p.toFixed(1)}%"></i></div>`;
}
function stateLabel(st) {
  return { 'aborted-before-start': 'aborted', interrupted: 'interrupted', violation: 'VIOLATION' }[st] || st;
}

/* ---------- fetch */
const etags = new Map();
const lastBody = new Map();
async function getJSON(url, useEtag) {
  const headers = {};
  if (useEtag && etags.has(url)) headers['If-None-Match'] = etags.get(url);
  const r = await fetch(url, { headers, cache: 'no-store' });
  if (r.status === 304) return { same: true, data: lastBody.get(url) };
  if (!r.ok) throw new Error(url + ': HTTP ' + r.status);
  const data = await r.json();
  if (useEtag) { etags.set(url, r.headers.get('ETag')); lastBody.set(url, data); }
  return { same: false, data };
}

/* ---------- connection */
let lastOk = 0;
function setConn(ok, text) {
  const c = $('#conn');
  c.className = 'conn ' + (ok ? 'ok' : 'bad');
  patch($('#conn-text'), esc(text));
}

/* ---------- overview */
let ov = null;
async function pollOverview() {
  try {
    const r = await getJSON('/api/overview', true);
    lastOk = Date.now();
    if (!r.same) { ov = r.data; renderOverview(); }
    setConn(true, 'live, ' + fmtTime(lastOk));
  } catch (e) {
    setConn(false, lastOk ? 'server stopped (run devdash.bat)' : 'cannot reach the server');
  }
  setTimeout(pollOverview, document.hidden ? 5000 : 1500);
}

function counter(cls, n, label) {
  return `<div class="counter ${n ? cls : 'zero'}"><b>${n}</b><span>${esc(label)}</span></div>`;
}

function renderOverview() {
  const c = ov.counts;
  patch($('#counters'),
    counter('run', c.tests_running, 'tests running') +
    counter('warn', c.tests_queued, 'queued') +
    counter('pass', c.passed_today, 'passed today') +
    counter('fail', c.failed_today, 'failed today') +
    counter('run', c.agents_running, 'agents running') +
    counter('run', c.workflows_running, c.workflows_running === 1 ? 'workflow' : 'workflows'));

  // alerts
  let al = '';
  for (const s of ov.other_supervisors) {
    if (s.kind === 'ctest-e2e') al += `<div class="alert">ctest e2e is running a test supervisor (pid ${s.pid}): a hidden run started now would be refused.</div>`;
    else if (s.kind === 'restore') al += `<div class="alert">A restore is running for ${esc(s.run || '')} (pid ${s.pid}).</div>`;
    else al += `<div class="alert">A test supervisor (pid ${s.pid}, ${esc(s.kind)}) is running${s.run ? ' for ' + esc(s.run) : ''}.</div>`;
  }
  for (const r of c.needs_restore || []) {
    const [rk, name] = r.split('/');
    al += `<div class="alert bad">Run <a href="#run/${enc(rk)}/${enc(name)}">${esc(name)}</a> needs a restore. Open it for the Restore-TestRun.ps1 command (devdash never runs it).</div>`;
  }
  patch($('#alerts'), al);

  // live tests
  patch($('#live-hint'), ov.live.length ? '' : (c.ds_running ? `${c.ds_running} ds.exe running outside a hidden run` : 'no hidden run right now'));
  keyed($('#live'), ov.live, (r) => r.rootkey + '/' + r.name, renderLive,
    'Nothing is running. Hidden runs appear here as soon as Start-TestRun.ps1 starts one.');

  // batches and tasks
  keyed($('#batches'), ov.batches, (t) => t.id, renderBatch, 'No batch. <span class="mono">pwsh -File tools\\testenv\\Run-Batch.ps1 -Scenarios a,b</span>');
  keyed($('#tasks'), ov.tasks, (t) => t.id, renderTask, 'No posted task.');
  $('#batches-panel').style.display = '';

  // research
  const running = ov.workflows.filter((w) => w.status === 'running' || w.status === 'stalled');
  const subs = ov.subagents || [];
  keyed($('#flows'), running.concat(subs.length ? [{ __subs: true }] : []), (w) => w.__subs ? '__subs' : w.runId,
    (w, el) => (w.__subs ? renderSubs(subs, el) : renderFlow(w, el)), 'No workflow running.');
  const done = ov.workflows.filter((w) => !(w.status === 'running' || w.status === 'stalled')).slice(0, 8);
  patch($('#flows-done'), done.length ? '<div class="compact">' + done.map((w) => `
    <div class="compact-row" data-wf="${esc(w.runId)}">
      <span class="name">${esc(w.name || w.runId)}</span>
      ${badge(w.status, w.status)}
      <span class="meta">${w.agents_count.done} agents</span>
      <span class="meta">${fmtDur((w.duration_ms || 0) / 1000)} &middot; ${esc(fmtAgo(w.ended))}</span>
    </div>`).join('') + '</div>' : '');

  // builds and checks
  let sys = '';
  for (const t of ov.ctest) {
    const pct = t.n ? (100 * t.k / t.n) : 0;
    sys += `<div class="sys-row"><b>ctest ${esc(t.build)}</b> ${badge(t.state === 'done' ? (t.failed.length ? 'fail' : 'pass') : (t.state === 'running' ? 'running' : 'stale'), t.state === 'done' ? (t.failed.length ? t.failed.length + ' failed' : 'passed') : t.state, t.state === 'running')}
      <span class="muted">${t.k}/${t.n}${t.current ? ' &middot; ' + esc(t.current) : ''}${t.failed.length ? ' &middot; failed: ' + esc(t.failed.join(', ')) : ''} &middot; ${esc(fmtAgo(t.updated))}</span></div>`;
    if (t.state === 'running') sys += `<div class="prog">${bar(pct)}</div>`;
  }
  sys += `<div class="sys-row muted">Runs today: <b>${c.runs_today}</b> (${c.passed_today} passed, ${c.failed_today} failed, ${c.refused_today} refused) &middot; ds.exe running: <b>${c.ds_running}</b></div>`;
  patch($('#system'), '<div class="sys">' + sys + '</div>');
}

function shotThumb(s, idx, group) {
  return `<button class="thumb" data-lb="${esc(group)}" data-i="${idx}" title="${esc(s.side + ': ' + s.name)}"><img loading="lazy" src="${esc(s.url)}" alt=""><span>${esc(s.side === 'guest' ? 'guest: ' : '')}${esc(s.name.replace(/\.png$/i, ''))}</span></button>`;
}
const lbGroups = new Map();

function renderLive(r, el) {
  if (!el._built) {
    el.className = 'card live clickable';
    el.innerHTML = '<div class="h"></div><div class="p"></div><div class="c"></div><div class="g"></div><div class="s"></div><pre class="tail"></pre>';
    el.addEventListener('click', (e) => { if (!e.target.closest('.thumb')) location.hash = `run/${el._rk}/${enc(el._name)}`; });
    el._built = true;
  }
  el._rk = r.rootkey; el._name = r.name;
  const role = r.two_games ? 'two games' : (r.role || '');
  patch($('.h', el), `<div class="card-head">${badge('live', r.state === 'starting' ? 'starting' : (r.phase || 'live'), true)}
    <span class="card-title">${esc(r.scenario)}</span>${role ? badge('ghost', role) : ''}<span class="spacer"></span>
    <span class="muted mono">${esc(r.name.slice(0, 15))}</span></div>
    ${r.subtitle ? `<div class="card-sub">${esc(r.subtitle)}</div>` : ''}`);
  const total = r.steps_total || 0;
  const done = r.steps_done || 0;
  const failed = (r.failed_lines || []).length;
  patch($('.p', el), `<div class="prog"><div class="prog-top"><span>step <b>${done}</b> of <b>${total || '?'}</b>${failed ? ` &middot; <span style="color:var(--fail)">${failed} failed</span>` : ''}</span>
    <span><b>${(r.pct || 0).toFixed(0)}%</b> &middot; ${fmtDur(r.elapsed_s)}${r.steps_limit_s ? ' of ' + fmtDur(r.steps_limit_s) + ' limit' : ''}</span></div>
    ${bar(r.pct, failed ? 'fail' : '')}</div>`);
  const ls = r.last_step;
  patch($('.c', el), `<div class="current"><span class="lab">now</span>${esc(r.current || '')}</div>` +
    (ls ? `<div class="laststep">last: <span class="${ls.ok ? 'ok' : 'bad'}">${ls.ok ? 'ok' : 'FAIL'}</span> line ${ls.line} <span class="mono">${esc(ls.what)}</span> (${ls.seconds}s): ${esc((ls.detail || '').slice(0, 220))}</div>` : ''));
  const games = Object.entries(r.games || {});
  patch($('.g', el), games.length ? '<div class="games">' + games.map(([side, g]) => {
    const st = g.status || {};
    return `<div class="game"><b>${esc(side)}</b> ${st.fps ? esc(st.fps) + ' fps' : ''} ${st.uptime_s ? '&middot; up ' + fmtDur(+st.uptime_s) : ''}
      ${st.commands ? '&middot; ' + esc(st.commands) + ' cmds' : ''} ${+st.failures ? '&middot; <span style="color:var(--fail)">' + esc(st.failures) + ' failed</span>' : ''}
      ${g.build ? '<div class="faint">' + esc(g.build) + '</div>' : ''}</div>`;
  }).join('') + '</div>' : '');
  const shots = (r.live_shots || []);
  const group = 'live:' + r.rootkey + '/' + r.name;
  lbGroups.set(group, shots.map((s) => ({ url: s.url, cap: s.side + ': ' + s.name })));
  const recent = shots.map((s, i) => [s, i]).slice(-10).reverse();
  patch($('.s', el), recent.length ? '<div class="strip">' + recent.map(([s, i]) => shotThumb(s, i, group)).join('') + '</div>' : '');
  const tail = $('.tail', el);
  const text = (r.sup_tail || []).join('\n');
  if (tail._t !== text) { tail.textContent = text; tail._t = text; requestAnimationFrame(() => { tail.scrollTop = tail.scrollHeight; }); }
}

function itemMeta(it) {
  if (it.run) {
    const r = it.run;
    if (r.state === 'live' || r.state === 'starting') return `step ${r.steps_done || 0}/${r.steps_total || '?'} &middot; ${(r.pct || 0).toFixed(0)}%`;
    const iss = (r.issues || []).length ? ' &middot; ' + esc(r.issues.join(', ')) : '';
    return `${fmtDur(r.duration_s)}${iss}`;
  }
  if (it.detail) return esc(it.detail.slice(0, 80));
  return '';
}

function renderBatch(t, el) {
  if (!el._built) {
    el.className = 'card';
    el.addEventListener('click', (e) => {
      const row = e.target.closest('.item.link');
      if (row) { location.hash = `run/${row.dataset.rk}/${enc(row.dataset.name)}`; return; }
      if (e.target.closest('.card-head')) location.hash = 'task/' + enc(el.dataset.key);
    });
    el._built = true;
  }
  const items = t.items || [];
  const n = t.items_count || {};
  const segs = items.length && items.length <= 60
    ? '<div class="segbar">' + items.map((i) => `<i class="${esc(i.status)}" title="${esc(i.key + ': ' + i.status)}"></i>`).join('') + '</div>'
    : bar(t.pct, n.fail ? 'fail' : (t.status === 'pass' ? 'pass' : ''));
  const st = t.status;
  patch(el, `<div class="card-head" style="cursor:pointer">${badge(st, st, st === 'running')}<span class="card-title">${esc(t.title)}</span>
      <span class="spacer"></span><span class="muted">${esc(fmtAgo(t.updated))}</span></div>
    <div class="prog"><div class="prog-top"><span><b>${t.done ?? 0}</b> / <b>${t.total ?? '?'}</b> done &middot;
      <span style="color:var(--pass)">${n.pass || 0} passed</span> &middot; <span style="color:var(--fail)">${n.fail || 0} failed</span>${n.skip ? ' &middot; ' + n.skip + ' skipped' : ''}</span>
      <span><b>${t.pct != null ? t.pct.toFixed(0) + '%' : ''}</b></span></div>${segs}</div>
    ${t.current ? `<div class="current"><span class="lab">now</span>${esc(t.current)}</div>` : ''}
    <div class="items">${items.map((i) => `<div class="item${i.run ? ' link' : ''}" ${i.run ? `data-rk="${esc(i.run.rootkey)}" data-name="${esc(i.run.name)}"` : ''}>
      ${badge(i.status, i.status, i.status === 'running')}<span class="name">${esc(i.title || i.key)}</span><span class="meta">${itemMeta(i)}</span></div>`).join('')}</div>
    ${t.summary ? `<div class="card-sub">${esc(t.summary)}</div>` : ''}`);
}

function renderTask(t, el) {
  if (!el._built) {
    el.className = 'card clickable';
    el.addEventListener('click', () => { location.hash = 'task/' + enc(el.dataset.key); });
    el._built = true;
  }
  const st = t.status;
  const indet = t.pct == null && st === 'running';
  patch(el, `<div class="card-head">${badge(st, st, st === 'running')}<span class="card-title">${esc(t.title)}</span>${badge('ghost', t.kind)}
      <span class="spacer"></span><span class="muted">${esc(fmtAgo(t.updated))}</span></div>
    ${(t.pct != null || indet) ? `<div class="prog"><div class="prog-top"><span>${t.total != null ? `<b>${t.done ?? 0}</b> / <b>${t.total}</b>` : 'working'}${t.note ? ' &middot; ' + esc(t.note) : ''}</span><span><b>${t.pct != null ? t.pct.toFixed(0) + '%' : ''}</b></span></div>${bar(t.pct, indet ? 'indet' : (st === 'fail' ? 'fail' : (st === 'pass' || st === 'done' ? 'pass' : '')))}</div>` : ''}
    ${t.last_log ? `<div class="laststep"><span class="faint">${esc(fmtTime(t.last_log.t))}</span> ${esc(t.last_log.msg.slice(0, 240))}</div>` : ''}
    ${t.summary ? `<div class="card-sub">${esc(t.summary)}</div>` : ''}`);
}

function renderFlow(w, el) {
  if (!el._built) {
    el.className = 'card clickable';
    el.addEventListener('click', () => { location.hash = 'wf/' + enc(el.dataset.key); });
    el._built = true;
  }
  const ac = w.agents_count;
  patch(el, `<div class="card-head">${badge(w.status, w.status, w.status === 'running')}<span class="card-title">${esc(w.name || w.runId)}</span>
      <span class="spacer"></span><span class="muted">${fmtDur((w.duration_ms || 0) / 1000)}</span></div>
    ${w.description ? `<div class="card-sub">${esc(w.description)}</div>` : ''}
    <div class="prog"><div class="prog-top"><span><b>${ac.running}</b> running &middot; <b>${ac.done}</b> done${ac.idle ? ' &middot; ' + ac.idle + ' idle' : ''}${ac.superseded ? ' &middot; ' + ac.superseded + ' retried' : ''}</span>
      <span title="approximate: agents per phase are not known in advance"><b>&asymp; ${w.pct_approx.toFixed(0)}%</b></span></div>${bar(w.pct_approx, 'approx')}</div>
    <div class="phases">${w.phases.map((p) => `<span class="phase ${esc(p.state)}" title="${esc(p.detail || '')}">${p.state === 'done' ? '&#x2713;' : (p.state === 'active' ? '&#x25CF;' : '&#x25CB;')} ${esc(p.title)}${p.started ? ` <small>${p.done}/${p.started}</small>` : ''}</span>`).join('')}</div>
    ${w.running_labels.length ? '<div class="labels">' + w.running_labels.map((l) => `<span class="chip run">${esc(l)}</span>`).join('') + '</div>' : ''}
    ${w.latest_results.length ? '<div class="results">' + w.latest_results.slice(0, 2).map((r) => `<div class="result"><b>${esc(r.label)}</b>${esc(r.text.slice(0, 200))}</div>`).join('') + '</div>' : ''}`);
}

function renderSubs(subs, el) {
  el.className = 'card';
  patch(el, `<div class="card-head">${badge('running', 'agents', true)}<span class="card-title">Subagents (not in a workflow)</span></div>
    <div class="labels">${subs.map((s) => `<span class="chip run" title="${esc(s.type || '')}">${esc(s.label)} &middot; ${esc(fmtAgo(s.last_activity))}</span>`).join('')}</div>`);
}

/* ---------- history */
const hist = { state: '', q: '', runs: [], next: null, loading: false };
async function loadHistory(more) {
  if (hist.loading) return;
  hist.loading = true;
  try {
    const limit = more ? 40 : Math.max(40, Math.min(500, hist.runs.length || 40));
    let url = `/api/runs?limit=${limit}`;
    if (hist.state) url += '&state=' + enc(hist.state);
    if (hist.q) url += '&scenario=' + enc(hist.q);
    if (more && hist.next) url += '&before=' + enc(hist.next);
    const { data } = await getJSON(url, false);
    hist.runs = more ? hist.runs.concat(data.runs) : data.runs;
    hist.next = data.next;
    renderHistory();
  } catch (e) { /* the connection badge says it */ }
  hist.loading = false;
}
function checks(r) {
  const v = r.verdict;
  if (r.state === 'refused') return `<span class="muted">${esc((r.refused || '').slice(0, 90))}</span>`;
  if (!v) return r.state === 'interrupted' ? badge('fail', 'no summary') : '';
  if (v.mode === 'probe') return badge('ghost', 'probe');
  const out = [];
  if (!v.issues.length) out.push(badge('pass', 'saves unchanged'), badge('pass', 'restored'), badge('pass', 'exit 0'));
  for (const i of v.issues) out.push(badge('fail', i));
  if (r.repaired_later) out.push(badge('warn', 'repaired later'));
  if (r.needs_restore) out.push(badge('fail', 'NEEDS RESTORE'));
  return '<div class="checks">' + out.join('') + '</div>';
}
function renderHistory() {
  const tb = $('#hist');
  keyed(tb, hist.runs, (r) => r.rootkey + '/' + r.name, (r, el) => {
    if (!el._built) {
      el.className = 'r';
      el.addEventListener('click', () => { location.hash = `run/${el._rk}/${enc(el._name)}`; });
      el._built = true;
    }
    el._rk = r.rootkey; el._name = r.name;
    const st = r.state === 'passed' && !r.ok ? 'warn' : r.state;
    patch(el, `<td>${badge(st, stateLabel(r.state), r.state === 'live')}</td>
      <td class="when">${esc(fmtWhen(r.stamp))}</td>
      <td><div class="scn">${esc(r.scenario)}${r.root && r.root !== 'build-testenv/runs' ? ' <span class="faint">(' + esc(r.root) + ')</span>' : ''}</div>${r.subtitle ? `<div class="sub" title="${esc(r.subtitle)}">${esc(r.subtitle)}</div>` : ''}</td>
      <td class="num">${r.steps_total ? `${r.steps_done}/${r.steps_total}` : (r.steps_done || '')}${(r.failed_lines || []).length ? ` <span style="color:var(--fail)">(${r.failed_lines.length} fail)</span>` : ''}</td>
      <td class="num">${fmtDur(r.state === 'live' ? r.elapsed_s : r.duration_s)}</td>
      <td>${checks(r)}</td>
      <td class="num">${r.shots || ''}</td>`);
  }, '<td colspan="7">No runs match.</td>', 'tr');
  $('#hist-more').style.display = hist.next ? '' : 'none';
}
function histLoop() { loadHistory(false); setTimeout(histLoop, document.hidden ? 30000 : 10000); }

/* ---------- lightbox */
const lb = { list: [], i: 0 };
function lbOpen(list, i) {
  lb.list = list; lb.i = i;
  $('#lightbox').classList.add('on');
  $('#lightbox').setAttribute('aria-hidden', 'false');
  lbShow();
}
function lbShow() {
  const it = lb.list[lb.i];
  if (!it) return;
  $('#lb-img').src = it.url;
  patch($('#lb-cap'), `<b>${esc(it.cap)}</b> &middot; ${lb.i + 1} / ${lb.list.length} &middot; <a href="${esc(it.url)}" target="_blank" rel="noopener">full size</a>`);
}
function lbStep(d) { if (lb.list.length) { lb.i = (lb.i + d + lb.list.length) % lb.list.length; lbShow(); } }
function lbClose() { $('#lightbox').classList.remove('on'); $('#lightbox').setAttribute('aria-hidden', 'true'); }

document.addEventListener('click', (e) => {
  const t = e.target.closest('.thumb[data-lb]');
  if (t) {
    e.stopPropagation(); e.preventDefault();
    const list = lbGroups.get(t.dataset.lb) || [];
    lbOpen(list, +t.dataset.i || 0);
    return;
  }
  const wf = e.target.closest('[data-wf]');
  if (wf) location.hash = 'wf/' + enc(wf.dataset.wf);
}, true);

/* ---------- drawer (run, workflow, task) */
const dr = { kind: null, key: null, timer: null, tab: 'steps', log: null, agent: null, data: null };
function drawerOpen(title) {
  patch($('#drawer-title'), title);
  $('#drawer').classList.add('on'); $('#drawer').setAttribute('aria-hidden', 'false');
  $('#scrim').classList.add('on');
}
function drawerClose() {
  clearTimeout(dr.timer); dr.kind = null; dr.key = null; dr.log = null; dr.agent = null;
  $('#drawer').classList.remove('on'); $('#drawer').setAttribute('aria-hidden', 'true');
  $('#scrim').classList.remove('on');
  if (location.hash) history.replaceState(null, '', location.pathname);
}
function onHash() {
  const h = decodeURIComponent(location.hash.slice(1));
  clearTimeout(dr.timer);
  let m;
  if ((m = h.match(/^run\/([0-9a-f]{10})\/(.+)$/))) openRun(m[1], m[2]);
  else if ((m = h.match(/^wf\/(wf_[\w-]+)$/))) openFlow(m[1]);
  else if ((m = h.match(/^task\/([\w.-]+)$/))) openTask(m[1]);
  else if ($('#drawer').classList.contains('on')) drawerClose();
}

/* run detail */
const LOG_NAMES = { supervisor: 'supervisor.log', host: 'host mod log', guest: 'guest mod log', results: 'host results', guest_results: 'guest results', host_stdout: 'host stdout', guest_stdout: 'guest stdout', scenario: 'scenario' };
function openRun(rk, name) {
  const key = rk + '/' + name;
  if (dr.key !== key) {
    dr.kind = 'run'; dr.key = key; dr.tab = dr.tab && ['steps', 'logs', 'shots', 'summary'].includes(dr.tab) ? dr.tab : 'steps';
    dr.log = { which: 'supervisor', offset: null, source: null, lines: [], filter: '', follow: true };
    $('#drawer-body').innerHTML = '<div id="rd-head"></div><div class="tabs" id="rd-tabs"></div><div id="rd-body"></div>';
    $('#drawer-body')._h = null;
  }
  drawerOpen(esc(name));
  pollRun(rk, name);
}
async function pollRun(rk, name) {
  const key = rk + '/' + name;
  if (dr.key !== key) return;
  // one poll loop per drawer: a newer call (a tab or log switch) makes an older one in flight stop after its fetch
  const gen = dr.pollGen = (dr.pollGen || 0) + 1;
  let d;
  try { d = (await getJSON(`/api/runs/${rk}/${enc(name)}`, false)).data; } catch (e) {
    if (dr.pollGen === gen) patch($('#rd-head'), '<div class="alert bad">This run is not known to devdash.</div>');
    return;
  }
  if (dr.key !== key || dr.pollGen !== gen) return;
  dr.data = d;
  renderRunHead(d);
  renderRunTabs(d);
  await renderRunBody(d);
  if (dr.key !== key || dr.pollGen !== gen) return;
  const live = d.state === 'live' || d.state === 'starting';
  if (live || dr.tab === 'logs') dr.timer = setTimeout(() => pollRun(rk, name), live ? 2000 : 10000);
}
function fact(k, v, cls) { return `<div class="fact"><div class="k">${esc(k)}</div><div class="v ${cls || ''}">${v}</div></div>`; }
function renderRunHead(d) {
  const v = d.verdict || {};
  const yes = (b, good) => b == null ? '<span class="faint">n/a</span>' : (b === good ? `<span class="good">${b}</span>` : `<span class="bad">${b}</span>`);
  let html = `<div class="card-head">${badge(d.state === 'passed' && !d.ok ? 'warn' : d.state, stateLabel(d.state), d.state === 'live')}
      <span class="card-title">${esc(d.scenario)}</span>${d.two_games ? badge('ghost', 'two games') : (d.role ? badge('ghost', d.role) : '')}
      <span class="spacer"></span><span class="muted">${esc(fmtWhen(d.stamp))} &middot; ${esc(d.root || '')}</span></div>
    ${d.subtitle ? `<div class="card-sub">${esc(d.subtitle)}</div>` : ''}`;
  if (d.state === 'live' || d.state === 'starting') {
    html += `<div class="prog"><div class="prog-top"><span>step <b>${d.steps_done || 0}</b> of <b>${d.steps_total || '?'}</b> &middot; ${esc(d.phase || '')}</span><span><b>${(d.pct || 0).toFixed(0)}%</b> &middot; ${fmtDur(d.elapsed_s)}</span></div>${bar(d.pct)}</div>`;
  }
  html += '<div class="facts">' +
    fact('steps', `${d.steps_done ?? ''}${d.steps_total ? ' / ' + d.steps_total : ''}${(d.failed_lines || []).length ? ` <span class="bad">(${d.failed_lines.length} failed)</span>` : ''}`) +
    fact('duration', esc(fmtDur(d.state === 'live' ? d.elapsed_s : d.duration_s))) +
    fact('result', esc(v.result ?? (d.refused ? 'refused' : 'n/a'))) +
    fact('game exit', yes(v.game_exit_code, 0)) +
    (d.two_games ? fact('guest exit', yes(v.guest_exit_code, 0)) : '') +
    fact('real saves unchanged', yes(v.saves_unchanged, true)) +
    fact('game folder restored', yes(v.restored, true)) +
    fact('violation', yes(v.violation, false)) +
    fact('cloud uploads', yes(v.cloud_uploads, 0)) +
    (v.game_server_writes !== undefined ? fact('game server writes', yes(v.game_server_writes, 0)) : '') +
    (v.online_blocked !== undefined ? fact('online calls blocked', esc(String(v.online_blocked))) : '') +
    (v.seed_saves ? fact('seed saves', esc(String(v.seed_saves).split(/[\\/]/).pop())) : '') +
    (v.guest_seed ? fact('guest seed', esc(v.guest_seed)) : '') +
    (v.launcher_ini ? fact('launcher ini', 'yes') : '') +
    '</div>';
  if (d.refused) html += `<div class="alert bad">Refused: ${esc(d.refused)}</div>`;
  if (d.violation_text) html += `<div class="alert bad">Safety violation: ${esc(d.violation_text)}</div>`;
  if (d.stop && d.stop !== 'completed') html += `<div class="alert">Scenario stopped: ${esc(d.stop)}</div>`;
  if (d.note) html += `<div class="alert">${esc(d.note)}</div>`;
  if (d.restore_cmd) html += `<div class="alert bad">This run needs a restore. Run it yourself once no ds.exe is running:<code class="cmd">${esc(d.restore_cmd)}</code></div>`;
  patch($('#rd-head'), html);
}
function renderRunTabs(d) {
  const tabs = [['steps', 'Steps', (d.steps || []).length], ['logs', 'Logs', ''], ['shots', 'Screenshots', (d.shots_list || []).length], ['summary', 'Details', '']];
  patch($('#rd-tabs'), tabs.map(([k, l, n]) => `<button class="tab${dr.tab === k ? ' on' : ''}" data-tab="${k}">${l}${n !== '' ? `<small>${n}</small>` : ''}</button>`).join(''));
}
async function renderRunBody(d) {
  const body = $('#rd-body');
  if (dr.tab === 'steps') {
    patch(body, '<table class="steps"><tbody>' + (d.steps || []).map((s) => `<tr class="${s.ok ? '' : 'fail'}${s.optional ? ' opt' : ''}">
        <td class="l">${s.line}</td><td>${s.ok ? badge('pass', 'ok') : badge('fail', 'FAIL')}</td><td class="what">${esc(s.what)}</td><td class="det">${esc(s.detail)}</td><td class="s">${s.seconds}s</td></tr>`).join('') +
      ((d.state === 'live' && d.current) ? `<tr><td class="l"></td><td>${badge('running', 'now', true)}</td><td class="what" colspan="3">${esc(d.current)}</td></tr>` : '') +
      '</tbody></table>' + (!(d.steps || []).length ? '<div class="empty">No steps recorded.</div>' : ''));
  } else if (dr.tab === 'shots') {
    // lightbox order = display order: the host's shots, then the guest's
    const shots = ['host', 'guest'].flatMap((side) => (d.shots_list || []).filter((s) => s.side === side));
    const group = 'run:' + d.rootkey + '/' + d.name;
    lbGroups.set(group, shots.map((s) => ({ url: s.url, cap: s.side + ': ' + s.name })));
    let html = '';
    for (const side of ['host', 'guest']) {
      const mine = shots.map((s, i) => [s, i]).filter(([s]) => s.side === side);
      if (!mine.length) continue;
      html += `<div class="gallery-group">${side} (${mine.length})</div><div class="gallery">` + mine.map(([s, i]) => shotThumb(s, i, group)).join('') + '</div>';
    }
    patch(body, html || `<div class="empty">${d.state === 'interrupted' ? 'This run was interrupted: its screenshots are gone.' : 'No screenshots.'}</div>`);
  } else if (dr.tab === 'summary') {
    const ex = d.summary_extra || {};
    let html = '';
    for (const [k, v] of Object.entries(ex)) html += `<div class="fact" style="margin-top:8px"><div class="k">${esc(k.replace(/_/g, ' '))}</div><div class="pre">${esc(typeof v === 'string' ? v : JSON.stringify(v, null, 1))}</div></div>`;
    const res = d.resources;
    if (res && res.processes) {
      html += '<div class="facts">' + res.processes.map((p) => fact(p.name + ' peak memory', `${fmtNum(p.peak_working_set_mb)} MB (GPU ${fmtNum(p.peak_gpu_dedicated_mb)} MB)`) + fact(p.name + ' CPU avg / peak', `${p.avg_cpu_pct}% / ${p.peak_cpu_pct}%`)).join('') + '</div>';
    }
    html += `<div class="fact" style="margin-top:8px"><div class="k">plan</div><div class="pre">${esc(JSON.stringify(d.plan || {}, null, 1))}</div></div>`;
    html += `<div class="fact" style="margin-top:8px"><div class="k">log segments</div><div class="pre">${esc((d.segments || []).map((s) => `${s.kind} pid ${s.pid ?? '-'}: ${s.final || 'no final line'} (${s.records} lines)`).join('\n'))}</div></div>`;
    patch(body, html);
  } else if (dr.tab === 'logs') {
    await renderLogs(d);
  }
}
async function renderLogs(d) {
  const body = $('#rd-body');
  const L = dr.log;
  if (!$('#logview', body)) {
    body.innerHTML = `<div class="logbar" id="logbar"></div><pre class="logview" id="logview"></pre>`;
    body._h = null;
    $('#logview').addEventListener('scroll', (e) => {
      const el = e.target;
      L.follow = el.scrollTop + el.clientHeight >= el.scrollHeight - 30;
    });
  }
  const files = d.files || {};
  const live = d.state === 'live';
  patch($('#logbar'), Object.keys(LOG_NAMES).filter((k) => files[k] || (live && ['host', 'guest', 'results', 'guest_results'].includes(k) && (k.startsWith('guest') ? d.two_games : true)) || k === L.which)
    .map((k) => `<button class="pill${L.which === k ? ' on' : ''}" data-log="${k}">${esc(LOG_NAMES[k])}</button>`).join('') +
    `<input class="input" id="logfilter" type="search" placeholder="filter" value="${esc(L.filter)}" spellcheck="false"><span class="faint" id="logsrc"></span>`);
  let r;
  const which = L.which, gen = L.gen || 0;
  try {
    let url = `/api/runs/${d.rootkey}/${enc(d.name)}/log?which=${which}&tail=1500`;
    if (L.offset != null) url += `&after=${L.offset}&source=${enc(L.source || '')}`;
    r = (await getJSON(url, false)).data;
  } catch (e) { return; }
  // the answer to a request for another log (the user switched logs meanwhile): its lines and offset are not this log's
  if (dr.log !== L || L.which !== which || (L.gen || 0) !== gen) return;
  if (r.missing) { L.lines = []; L.offset = null; } else {
    if (r.reset) L.lines = r.lines; else L.lines = L.lines.concat(r.lines);
    if (L.lines.length > 6000) L.lines = L.lines.slice(-6000);
    L.offset = r.offset; L.source = r.source;
  }
  patch($('#logsrc'), r.missing ? 'not available' : (r.source === 'game-folder' ? 'live from the game folder' : '') + (r.truncated_head ? ' (tail)' : ''));
  if (r.reset || r.lines.length || r.missing) drawLog();
}
function lineClass(s) {
  if (/\bFAIL\b|\bERROR\b|\[error\]|REFUSED|VIOLATION|exception|\b[1-9]\d* (failed|failures?|errors?)\b|CHANGE\(S\) in the real/.test(s)) return 'e';
  if (/\bWARN|\[warn\]|INCOMPLETE|timed out/i.test(s)) return 'w';
  if (/ -> ok \(|PASSED|CONFIRMED|UNCHANGED/.test(s)) return 'ok';
  return '';
}
function drawLog() {
  const L = dr.log;
  const pre = $('#logview');
  if (!pre) return;
  const f = L.filter.toLowerCase();
  const lines = f ? L.lines.filter((s) => s.toLowerCase().includes(f)) : L.lines;
  pre.innerHTML = lines.length ? lines.map((s) => { const c = lineClass(s); return c ? `<span class="${c}">${esc(s)}</span>` : esc(s); }).join('\n') : '<span class="faint">(empty)</span>';
  if (L.follow) pre.scrollTop = pre.scrollHeight;
}

/* workflow detail */
function openFlow(id) {
  if (dr.key !== 'wf:' + id) {
    dr.kind = 'wf'; dr.key = 'wf:' + id; dr.agent = null;
    $('#drawer-body').innerHTML = '<div id="wf-head"></div><div id="wf-agents" style="margin-top:14px"></div><div id="wf-events"></div>';
  }
  drawerOpen(esc(id));
  pollFlow(id);
}
async function pollFlow(id) {
  if (dr.key !== 'wf:' + id) return;
  let w;
  try { w = (await getJSON('/api/workflows/' + enc(id), false)).data; } catch (e) { patch($('#wf-head'), '<div class="alert bad">Unknown workflow.</div>'); return; }
  if (dr.key !== 'wf:' + id) return;
  patch($('#drawer-title'), esc(w.name || id));
  const ac = w.agents_count;
  patch($('#wf-head'), `<div class="card-head">${badge(w.status, w.status, w.status === 'running')}<span class="card-title">${esc(w.name || id)}</span>
      <span class="spacer"></span><span class="muted">${esc(w.session_title || '')} &middot; started ${esc(fmtWhen(w.started))}</span></div>
    ${w.description ? `<div class="card-sub">${esc(w.description)}</div>` : ''}
    <div class="prog"><div class="prog-top"><span><b>${ac.running}</b> running &middot; <b>${ac.done}</b> done${ac.aborted ? ' &middot; ' + ac.aborted + ' aborted' : ''}${ac.superseded ? ' &middot; ' + ac.superseded + ' retried' : ''}</span><span><b>&asymp; ${w.pct_approx.toFixed(0)}%</b> &middot; ${fmtDur((w.duration_ms || 0) / 1000)}</span></div>${bar(w.pct_approx, w.status === 'completed' ? 'pass' : 'approx')}</div>
    <div class="phases">${w.phases.map((p) => `<span class="phase ${esc(p.state)}" title="${esc(p.detail || '')}">${p.state === 'done' ? '&#x2713;' : (p.state === 'active' ? '&#x25CF;' : '&#x25CB;')} ${esc(p.title)}${p.started ? ` <small>${p.done}/${p.started}</small>` : ''}</span>`).join('')}</div>
    <div class="facts">${fact('agents', ac.total)}${w.tokens != null ? fact('tokens', fmtNum(w.tokens)) : ''}${w.tool_calls != null ? fact('tool calls', w.tool_calls) : ''}${w.ended ? fact('ended', esc(fmtWhen(w.ended))) : ''}</div>
    ${w.error ? `<div class="alert bad">${esc(w.error)}</div>` : ''}`);
  const agents = (w.agents || []).slice().reverse();
  patch($('#wf-agents'), `<div class="table-wrap"><table class="agents"><thead><tr><th>Agent</th><th>Phase</th><th>State</th><th>Started</th><th>Time</th><th>Now / result</th></tr></thead><tbody>` +
    agents.map((a) => `<tr class="a${dr.agent === a.agentId ? ' sel' : ''}" data-agent="${esc(a.agentId)}">
      <td class="lab">${esc(a.label)}</td><td>${esc(a.phase || '')}</td><td>${badge(a.state, a.state, a.state === 'running')}</td>
      <td class="muted">${esc(fmtTime(a.started))}</td><td class="muted">${fmtDur((a.duration_ms || 0) / 1000)}</td>
      <td class="prev">${a.current_tool ? `<span class="mono">${esc(a.current_tool.name)}</span> ${esc(a.current_tool.summary)}` : esc((a.result_preview || '').slice(0, 300))}</td></tr>`).join('') +
    '</tbody></table></div>');
  if (dr.agent) await pollAgent(id, dr.agent);
  if (w.status === 'running') dr.timer = setTimeout(() => pollFlow(id), 3000);
}
async function pollAgent(id, aid) {
  let r;
  try { r = (await getJSON(`/api/workflows/${enc(id)}/agents/${enc(aid)}?tail=80`, false)).data; } catch (e) { return; }
  if (dr.agent !== aid) return;
  patch($('#wf-events'), `<h2 style="margin-top:16px">Agent activity</h2><div class="events">` + r.events.slice().reverse().map((e) =>
    `<div class="ev ${esc(e.kind)}"><span class="t">${esc(fmtTime(e.t))}</span><span class="n">${esc(e.kind === 'tool' ? e.name : e.kind)}</span><span class="s">${esc(e.summary)}</span></div>`).join('') + '</div>');
}

/* task detail */
function openTask(id) {
  if (dr.key !== 'task:' + id) {
    dr.kind = 'task'; dr.key = 'task:' + id;
    $('#drawer-body').innerHTML = '<div id="tk-head"></div><div id="tk-items"></div><div id="tk-log"></div>';
  }
  drawerOpen(esc(id));
  pollTask(id);
}
async function pollTask(id) {
  if (dr.key !== 'task:' + id) return;
  let t;
  try { t = (await getJSON('/api/tasks/' + enc(id), false)).data; } catch (e) { patch($('#tk-head'), '<div class="alert bad">Unknown task.</div>'); return; }
  if (dr.key !== 'task:' + id) return;
  patch($('#drawer-title'), esc(t.title));
  const n = t.items_count || {};
  patch($('#tk-head'), `<div class="card-head">${badge(t.status, t.status, t.status === 'running')}<span class="card-title">${esc(t.title)}</span>${badge('ghost', t.kind)}
     <span class="spacer"></span><span class="muted">started ${esc(fmtWhen(t.started))} &middot; updated ${esc(fmtAgo(t.updated))}</span></div>
    ${t.pct != null ? `<div class="prog"><div class="prog-top"><span><b>${t.done}</b> / <b>${t.total}</b> &middot; ${n.pass || 0} passed, ${n.fail || 0} failed</span><span><b>${t.pct.toFixed(0)}%</b></span></div>${bar(t.pct, n.fail ? 'fail' : '')}</div>` : ''}
    ${t.summary ? `<div class="card-sub">${esc(t.summary)}</div>` : ''}${t.owner ? `<div class="card-sub">owner: ${esc(t.owner)}</div>` : ''}`);
  const items = t.items || [];
  patch($('#tk-items'), items.length ? `<div class="items" style="margin-top:14px">` + items.map((i) => `<div class="item${i.run ? ' link' : ''}" ${i.run ? `data-rk="${esc(i.run.rootkey)}" data-name="${esc(i.run.name)}"` : ''}>
      ${badge(i.status, i.status, i.status === 'running')}<span class="name">${esc(i.title || i.key)}${i.run ? ' <span class="faint mono">' + esc(i.run.name) + '</span>' : ''}</span><span class="meta">${itemMeta(i)}</span></div>`).join('') + '</div>' : '');
  patch($('#tk-log'), (t.log || []).length ? `<h2 style="margin-top:16px">Log</h2><pre class="logview" style="height:auto;max-height:50vh">` +
    t.log.map((l) => `<span class="${l.level === 'error' ? 'e' : (l.level === 'warn' ? 'w' : '')}">${esc(fmtTime(l.t))}  ${esc(l.msg)}</span>`).join('\n') + '</pre>' : '');
  if (t.status === 'running' || t.status === 'stale') dr.timer = setTimeout(() => pollTask(id), 2000);
}

/* ---------- events */
document.addEventListener('DOMContentLoaded', () => {
  $('#drawer-close').addEventListener('click', drawerClose);
  $('#scrim').addEventListener('click', drawerClose);
  $('#lb-prev').addEventListener('click', () => lbStep(-1));
  $('#lb-next').addEventListener('click', () => lbStep(1));
  $('#lb-close').addEventListener('click', lbClose);
  $('#lightbox').addEventListener('click', (e) => { if (e.target.id === 'lightbox') lbClose(); });
  $('#drawer-body').addEventListener('click', (e) => {
    const tab = e.target.closest('[data-tab]');
    if (tab && dr.data) { dr.tab = tab.dataset.tab; clearTimeout(dr.timer); renderRunTabs(dr.data); $('#rd-body').innerHTML = ''; $('#rd-body')._h = null; pollRun(dr.data.rootkey, dr.data.name); return; }
    const lg = e.target.closest('[data-log]');
    if (lg && dr.log) { dr.log.which = lg.dataset.log; dr.log.gen = (dr.log.gen || 0) + 1; dr.log.offset = null; dr.log.lines = []; dr.log.follow = true; clearTimeout(dr.timer); pollRun(dr.data.rootkey, dr.data.name); return; }
    const ag = e.target.closest('tr[data-agent]');
    if (ag) { dr.agent = ag.dataset.agent; clearTimeout(dr.timer); pollFlow(dr.key.slice(3)); return; }
    const it = e.target.closest('.item.link');
    if (it) location.hash = `run/${it.dataset.rk}/${enc(it.dataset.name)}`;
  });
  $('#drawer-body').addEventListener('input', (e) => {
    if (e.target.id === 'logfilter' && dr.log) { dr.log.filter = e.target.value; drawLog(); }
  });
  $('#hist-filters').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-state]');
    if (!b) return;
    $$('#hist-filters .pill').forEach((p) => p.classList.toggle('on', p === b));
    hist.state = b.dataset.state; hist.runs = []; loadHistory(false);
  });
  let st;
  $('#hist-search').addEventListener('input', (e) => { clearTimeout(st); st = setTimeout(() => { hist.q = e.target.value.trim(); hist.runs = []; loadHistory(false); }, 250); });
  $('#hist-more').addEventListener('click', () => loadHistory(true));
  document.addEventListener('keydown', (e) => {
    if ($('#lightbox').classList.contains('on')) {
      if (e.key === 'ArrowLeft') { lbStep(-1); e.preventDefault(); }
      else if (e.key === 'ArrowRight') { lbStep(1); e.preventDefault(); }
      else if (e.key === 'Escape') lbClose();
      return;
    }
    if (e.key === 'Escape' && $('#drawer').classList.contains('on')) drawerClose();
  });
  window.addEventListener('hashchange', onHash);
  pollOverview();
  histLoop();
  onHash();
});
