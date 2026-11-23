// voicecut page (docs/voicecut.md). No libraries: fetch, Web Audio and canvas.
'use strict';

const $ = (id) => document.getElementById(id);
const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
const fmt = (s, d = 3) => (s == null || isNaN(s)) ? '-' : Number(s).toFixed(d);
const dbText = (v) => (v == null || v <= -119) ? '-' : v.toFixed(1);

const S = {
  catalog: null, items: [], byId: new Map(), cats: [], catLabel: {}, sitByHash: new Map(),
  user: { clips: [], situations: {}, measured: {} },
  status: null, libShown: 300, covOpen: null, sitOpen: null,
  cur: null, buf: null, abuf: null, rate: 48000, mip: null,
  view: { a: 0, b: 1 }, sel: null, playhead: 0, handle: 'in',
  play: null, editing: null, loop: false,
};
const NORMALIZE_PEAK_DB = -3.0;   // vc_audio.NORMALIZE_PEAK_DB
const MIP = 256;

// ------------------------------------------------------------------ server
async function getJSON(url) {
  const r = await fetch(url, { cache: 'no-store' });
  const j = await r.json().catch(() => ({ error: 'bad answer' }));
  if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}
async function post(url, body) {
  const r = await fetch(url, { method: 'POST', headers: { 'Content-Type': 'application/json', 'X-Voicecut': '1' },
    body: JSON.stringify(body || {}) });
  const j = await r.json().catch(() => ({ error: 'bad answer' }));
  if (!r.ok) throw new Error(j.error || ('HTTP ' + r.status));
  return j;
}
function alertBox(msg, kind = 'error', ms = 0) {
  const a = el('div', 'alert ' + kind);
  a.appendChild(el('span', null, msg));
  const x = el('button', 'icon x', '×'); x.onclick = () => a.remove(); a.appendChild(x);
  $('alerts').appendChild(a);
  if (ms) setTimeout(() => a.remove(), ms);
}

// ------------------------------------------------------------------ categories
function samSitCat(h) { const s = S.sitByHash.get(h); return S.user.situations[String(h)] || (s ? s.guess : 'other'); }
function itemCats(it) {
  if (it.speaker === 'sam' && it.situations && it.situations.length) return [...new Set(it.situations.map(samSitCat))];
  if (it.speaker === 'sam') return [];
  return it.category ? [it.category] : [];
}
function itemTitle(it) {
  if (it.kind === 'cutscene') return 'Cutscene ' + it.name + ' (' + it.lines + ' Fragile line' + (it.lines === 1 ? '' : 's') + ')';
  if (it.text) return it.text.replace(/\s+/g, ' ');
  const sit = (it.situations || []).map((h) => (S.sitByHash.get(h) || {}).label).filter(Boolean);
  return '(non-verbal)' + (sit.length ? ' · ' + sit[0] : '');
}
function itemLength(it) { const m = S.user.measured[it.id]; return m ? m.length : it.length; }

// ------------------------------------------------------------------ status
async function pollStatus() {
  try {
    const st = await getJSON('/api/status');
    S.status = st;
    const c = st.catalog;
    const box = $('status');
    box.textContent = '';
    if (c.state === 'building') {
      box.appendChild(document.createTextNode('Reading the game: ' + c.message));
      if (c.total) { const b = el('span', 'bar'); const i = el('i'); i.style.width = Math.round(100 * c.done / c.total) + '%'; b.appendChild(i); box.appendChild(b); }
    } else if (c.state === 'error') {
      box.textContent = 'Could not read the game';
      if (!S.shownError) { alertBox(c.error); S.shownError = true; }
    } else if (c.state === 'ready') {
      const dec = Object.entries(st.decoding || {});
      let t = c.message + ' · ' + st.clips + ' clip' + (st.clips === 1 ? '' : 's');
      if (dec.length) t += ' · decoding ' + Math.round(100 * dec[0][1]) + '%';
      if (st.measuring) t += ' · measuring ' + st.measuring + ' of Sam\'s lines';
      const an = st.analysis || {};
      if (an.state === 'running') t += ' · measuring sounds for suggestions ' + an.done + ' / ' + an.total;
      if (an.state === 'done' && S.user && !S.user.analysed) { await loadUser(); }
      const pf = st.prefetch || {};
      if (pf.state === 'running' || pf.state === 'paused') {
        t += ' · decoding every line ' + pf.done + ' / ' + pf.total + (pf.state === 'paused' ? ' (paused)' : '');
      } else if (pf.state === 'done' && pf.failed) t += ' · ' + pf.failed + ' lines did not decode (voicecut.log)';
      box.textContent = t;
      const pb = $('btn-prefetch');
      pb.classList.toggle('hidden', !(pf.state === 'running' || pf.state === 'paused'));
      pb.textContent = pf.state === 'paused' ? 'Resume decoding' : 'Pause decoding';
      pb.dataset.state = pf.state || '';
      if (!S.catalog) await loadCatalog();
      if (st.measuring || S.wasMeasuring) { S.wasMeasuring = !!st.measuring; await loadUser(); }
      else if ((pf.state === 'running' || S.wasPrefetching) && Date.now() - (S.lightAt || 0) > 8000) {
        // new lengths and loudness from the background decode: refresh what shows them, not the lists you scroll
        S.wasPrefetching = pf.state === 'running'; S.lightAt = Date.now();
        try { S.user.measured = (await getJSON('/api/user')).measured; renderGuide(); updateMeter(); } catch (e) { /* next time */ }
      }
    }
    const busy = c.state === 'building' || Object.keys(st.decoding || {}).length || st.measuring || (st.prefetch || {}).state === 'running';
    S.failures = 0;
    setTimeout(pollStatus, busy ? 700 : 4000);
  } catch (e) {
    S.failures = (S.failures || 0) + 1;
    if (S.failures >= 2) $('status').textContent = 'voicecut is not running (start voicecut.bat again)';
    setTimeout(pollStatus, S.failures >= 2 ? 5000 : 1000);
  }
}

async function loadCatalog() {
  const c = await getJSON('/api/catalog');
  if (!c.ready) return;
  S.catalog = c;
  S.items = c.items;
  S.byId = new Map(c.items.map((i) => [i.id, i]));
  S.cats = c.categories;
  S.catLabel = Object.fromEntries(c.categories.map((x) => [x.key, x.label]));
  S.sitByHash = new Map(c.situations.map((s) => [s.hash, s]));
  for (const sel of [$('f-cat'), $('c-cat')]) {
    for (const cat of c.categories) { const o = el('option', null, cat.label); o.value = cat.key; o.title = cat.hint; sel.appendChild(o); }
  }
  $('c-cat').value = 'stumble';
  await loadUser();
}
async function loadUser() {
  S.user = await getJSON('/api/user');
  $('n-clips').textContent = S.user.clips.length;
  renderLibrary(); renderClips(); renderCoverage(); renderSam(); renderGuide(); updateMeter();
}

// ------------------------------------------------------------------ library
function filtered() {
  const q = $('f-search').value.trim().toLowerCase();
  const sp = $('f-speaker').value, kind = $('f-kind').value, cat = $('f-cat').value, voice = $('f-voice').value;
  const mn = parseFloat($('f-min').value), mx = parseFloat($('f-max').value);
  let out = S.items.filter((it) => {
    if (sp && it.speaker !== sp) return false;
    if (kind && it.kind !== kind) return false;
    if (voice === 'nonverbal' && (it.text || it.kind === 'cutscene')) return false;
    if (voice === 'spoken' && !it.text) return false;
    const len = itemLength(it);
    if (!isNaN(mn) && len && len < mn) return false;
    if (!isNaN(mx) && len && len > mx) return false;
    if (cat && !itemCats(it).includes(cat)) return false;
    if (q) {
      const sits = (it.situations || []).map((h) => (S.sitByHash.get(h) || {}).label || '').join(' ');
      if (!((it.text || '') + ' ' + (it.name || '') + ' ' + it.group + ' ' + sits).toLowerCase().includes(q)) return false;
    }
    return true;
  });
  const sort = $('f-sort').value;
  if (sort === 'length') out.sort((a, b) => itemLength(a) - itemLength(b));
  else if (sort === 'length-desc') out.sort((a, b) => itemLength(b) - itemLength(a));
  else if (sort === 'text') out.sort((a, b) => itemTitle(a).localeCompare(itemTitle(b)));
  return out;
}
function renderLibrary() {
  if (!S.catalog) return;
  const list = $('lib-list');
  const rows = filtered();
  $('lib-count').textContent = rows.length + ' of ' + S.items.length;
  list.textContent = '';
  for (const it of rows.slice(0, S.libShown)) list.appendChild(libRow(it));
  if (rows.length > S.libShown) {
    const m = el('div', 'more'); const b = el('button', 'btn ghost small', 'Show ' + Math.min(300, rows.length - S.libShown) + ' more');
    b.onclick = () => { S.libShown += 300; renderLibrary(); }; m.appendChild(b); list.appendChild(m);
  }
  if (!rows.length) list.appendChild(el('div', 'empty', 'Nothing matches the filters.'));
}
function libRow(it) {
  const r = el('div', 'item' + (S.cur && S.cur.id === it.id ? ' active' : ''));
  r.appendChild(el('span', 'who ' + it.speaker, it.speaker === 'sam' ? 'S' : 'F'));
  const t = el('div', 'txt');
  t.appendChild(el('div', 'line1', itemTitle(it)));
  const l2 = el('div', 'line2');
  const cats = itemCats(it);
  for (const c of cats.slice(0, 2)) l2.appendChild(el('span', 'tag' + (it.sure ? '' : ' guess'), S.catLabel[c] || c));
  l2.appendChild(document.createTextNode(it.kind === 'cutscene' ? ((it.text || '').slice(0, 120)) : it.group));
  t.appendChild(l2);
  r.appendChild(t);
  const len = itemLength(it);
  r.appendChild(el('span', 'len', len ? fmt(len, 2) + ' s' : ''));
  r.onclick = () => openItem(it.id);
  r.title = it.text || '';
  return r;
}

// ------------------------------------------------------------------ audio
let ctx = null;
function audio() {
  if (!ctx) ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 48000 });
  if (ctx.state === 'suspended') ctx.resume();
  return ctx;
}
const bufCache = new Map();
async function fetchBuffer(id) {
  if (bufCache.has(id)) { const b = bufCache.get(id); bufCache.delete(id); bufCache.set(id, b); return b; }
  const r = await fetch('/api/audio?id=' + encodeURIComponent(id));
  if (!r.ok) { const j = await r.json().catch(() => ({})); throw new Error(j.error || ('HTTP ' + r.status)); }
  const ab = await audio().decodeAudioData(await r.arrayBuffer());
  bufCache.set(id, ab);
  while (bufCache.size > 40) bufCache.delete(bufCache.keys().next().value);
  return ab;
}
function stopAll() {
  S.seqToken = null;
  if (S.play) { try { S.play.node.onended = null; S.play.node.stop(); } catch (e) { /* already stopped */ } S.play = null; }
  $('b-play').innerHTML = '&#9654; Play';
}
function playBuffer(ab, opts) {
  stopAll();
  const a = audio();
  const node = a.createBufferSource();
  node.buffer = ab;
  node.loop = !!opts.loop;
  node.connect(a.destination);
  const offset = opts.offset || 0;
  node.start(0, offset);
  S.play = { node, t0: a.currentTime, offset, mode: opts.mode, dur: ab.duration, base: opts.base || 0 };
  node.onended = () => { if (S.play && S.play.node === node) { S.play = null; $('b-play').innerHTML = '&#9654; Play'; draw(); } };
  if (opts.mode === 'src' || opts.mode === 'sel') $('b-play').innerHTML = '&#9632; Stop';
  tick();
}
function tick() {
  if (!S.play) return;
  if (S.play.mode === 'src' || S.play.mode === 'sel') {
    let t = ctx.currentTime - S.play.t0;
    if (S.play.mode === 'sel') { t = S.play.node.loop ? t % S.play.dur : Math.min(t, S.play.dur); S.playhead = S.play.base + t; }
    else S.playhead = Math.min(S.play.offset + t, S.play.dur);
    draw();
  }
  requestAnimationFrame(tick);
}

// The clip render: the same math as vc_audio.render (trim, normalise, gain, raised-cosine fades, clamp).
function renderClip(src, rate, start, end, f) {
  const a = Math.max(0, Math.round(start * rate)), b = Math.min(src.length, Math.round(end * rate));
  const n = Math.max(0, b - a);
  const out = new Float32Array(n);
  if (!n) return out;
  let k = Math.pow(10, (f.gain_db || 0) / 20);
  if (f.normalize) {
    let peak = 0; for (let i = a; i < b; i++) { const v = Math.abs(src[i]); if (v > peak) peak = v; }
    if (peak > 1e-6) k *= Math.pow(10, NORMALIZE_PEAK_DB / 20) / peak;
  }
  const fi = Math.min(n, Math.round((f.fade_in || 0) * rate)), fo = Math.min(n, Math.round((f.fade_out || 0) * rate));
  for (let i = 0; i < n; i++) {
    let g = k;
    if (fi && i < fi) g *= 0.5 - 0.5 * Math.cos(Math.PI * (i + 0.5) / fi);
    if (fo && i >= n - fo) g *= 0.5 - 0.5 * Math.cos(Math.PI * (n - i - 0.5) / fo);
    const x = src[a + i] * g;
    out[i] = x > 1 ? 1 : (x < -1 ? -1 : x);
  }
  return out;
}
// vc_audio.stats: peak, and the loudness of the 20 ms windows within 30 dB of the loudest
function clipStats(x, rate) {
  const n = x.length;
  if (!n) return { peak_db: -120, loud_db: -120, length: 0 };
  let peak = 0; for (let i = 0; i < n; i++) { const v = Math.abs(x[i]); if (v > peak) peak = v; }
  const win = Math.max(1, Math.floor(rate / 50)); const wins = [];
  for (let i = 0; i < n; i += win) { let s = 0; const e = Math.min(n, i + win); for (let j = i; j < e; j++) s += x[j] * x[j]; wins.push(s / (e - i)); }
  const top = Math.max(...wins); const keep = wins.filter((e) => e >= top * 1e-3);
  const loud = keep.length ? Math.sqrt(keep.reduce((p, q) => p + q, 0) / keep.length) : 0;
  const d = (v) => v > 1e-6 ? 20 * Math.log10(v) : -120;
  return { peak_db: d(peak), loud_db: d(loud), length: n / rate };
}
function bufferOf(samples, rate) {
  const ab = audio().createBuffer(1, Math.max(1, samples.length), rate);
  ab.copyToChannel(samples, 0);
  return ab;
}
function form() {
  return { fade_in: (parseFloat($('c-fin').value) || 0) / 1000, fade_out: (parseFloat($('c-fout').value) || 0) / 1000,
    gain_db: parseFloat($('c-gain').value) || 0, normalize: $('c-norm').checked,
    category: $('c-cat').value, note: $('c-note').value };
}

// ------------------------------------------------------------------ editor
async function openItem(id, then) {
  const it = S.byId.get(id);
  if (!it) return;
  stopAll();
  S.cur = it; S.buf = null; S.abuf = null; S.sel = null; S.playhead = 0; S.subIdx = null;
  if (!then) setEditing(null);
  $('ed-title').textContent = (it.speaker === 'sam' ? 'Sam' : 'Fragile') + ' · ' + (it.kind === 'cutscene' ? 'cutscene ' + it.name : it.group);
  const src = $('ed-source'); src.textContent = '';
  src.classList.toggle('tall', it.kind === 'cutscene');
  if (it.kind === 'cutscene') {
    const subs = fragileSubs(it);
    if (subs.length) {
      src.appendChild(el('div', null, 'Every voice in this cutscene. Fragile\'s ' + subs.length + ' lines (click to jump, N / P next / previous; the strip under the waveform shows who speaks when):'));
      subs.forEach((s, k) => {
        const b = el('button', 'jump', fmtTime(s.t0) + '  ' + s.text.replace(/\s+/g, ' '));
        b.title = 'Select and zoom to this line'; b.onclick = () => jumpToSub(k);
        src.appendChild(b);
      });
    } else {
      src.appendChild(el('div', null, 'Every voice in this cutscene. Fragile says:'));
      for (const t of (it.text || '').split(' / ')) src.appendChild(el('div', 'faint', '• ' + t));
    }
  } else {
    src.textContent = itemTitle(it);
    const sits = (it.situations || []).map((h) => (S.sitByHash.get(h) || {}).label).filter(Boolean);
    if (sits.length) src.appendChild(el('div', 'faint', 'Sam plays this in: ' + sits.join(', ')));
  }
  renderLibrary();
  $('wave-msg').textContent = it.kind === 'cutscene' ? 'Decoding the cutscene’s dialogue track (about 10 s per minute, once)…' : 'Loading…';
  draw();
  try {
    const ab = await fetchBuffer(id);
    if (S.cur !== it) return;
    S.abuf = ab; S.buf = ab.getChannelData(0); S.rate = ab.sampleRate;
    S.mip = buildMip(S.buf);
    S.view = { a: 0, b: ab.duration };
    if (ab.duration <= 6 && !then) S.sel = { a: 0, b: ab.duration };
    $('wave-msg').textContent = '';
    if (then) then();
    draw(); updateMeter();
  } catch (e) {
    if (S.cur === it) $('wave-msg').textContent = 'Could not load this line: ' + e.message;
  }
}
// ---- cutscene subtitles: who speaks when (the game's subtitle timeline, 60 frames a second)
function fmtTime(t) { const m = Math.floor(t / 60); return m + ':' + (t - m * 60).toFixed(1).padStart(4, '0'); }
function fragileSubs(it) { return (it && it.subs || []).filter((s) => s.who === 'fragile'); }
function jumpToSub(k) {
  const subs = fragileSubs(S.cur);
  if (!subs.length || !S.buf) return;
  k = (k + subs.length) % subs.length;
  S.subIdx = k;
  const s = subs[k];
  // the subtitle span, a little wider at the end: the voice often runs past it
  S.sel = { a: Math.max(0, s.t0 - 0.1), b: Math.min(dur(), s.t1 + 0.4) }; S.handle = 'in';
  S.playhead = S.sel.a;
  for (const [i, b] of [...document.querySelectorAll('#ed-source .jump')].entries()) b.classList.toggle('on', i === k);
  const btn = document.querySelectorAll('#ed-source .jump')[k]; if (btn) btn.scrollIntoView({ block: 'nearest' });
  zoomSel(); selChanged();
}
function nextSub(dir) {
  const subs = fragileSubs(S.cur);
  if (!subs.length) return;
  if (S.subIdx == null) {   // from the playhead
    const t = S.playhead; let k = subs.findIndex((s) => s.t0 > t + 0.01);
    if (dir < 0) k = (k < 0 ? subs.length : k) - 1; else if (k < 0) k = 0;
    jumpToSub(k); return;
  }
  jumpToSub(S.subIdx + dir);
}
function buildMip(x) {
  const n = Math.ceil(x.length / MIP); const mn = new Float32Array(n), mx = new Float32Array(n);
  for (let b = 0; b < n; b++) {
    let lo = 1, hi = -1; const e = Math.min(x.length, (b + 1) * MIP);
    for (let i = b * MIP; i < e; i++) { const v = x[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    mn[b] = lo; mx[b] = hi;
  }
  return { mn, mx };
}
function dur() { return S.abuf ? S.abuf.duration : 0; }
function sizeCanvas(c) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(10, Math.round(c.clientWidth * dpr)), h = Math.max(10, Math.round(c.clientHeight * dpr));
  if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
  return [w, h, dpr];
}
function columns(x, a, b, w) {
  // min/max per pixel column for samples [a, b)
  const mn = new Float32Array(w), mx = new Float32Array(w);
  const per = (b - a) / w;
  for (let px = 0; px < w; px++) {
    const s0 = Math.floor(a + px * per), s1 = Math.max(s0 + 1, Math.floor(a + (px + 1) * per));
    let lo = 1, hi = -1;
    if (per > MIP * 2) {
      for (let k = Math.floor(s0 / MIP); k < Math.min(S.mip.mn.length, Math.ceil(s1 / MIP)); k++) { if (S.mip.mn[k] < lo) lo = S.mip.mn[k]; if (S.mip.mx[k] > hi) hi = S.mip.mx[k]; }
    } else {
      for (let i = Math.max(0, s0); i < Math.min(x.length, s1); i++) { const v = x[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    }
    if (lo > hi) { lo = 0; hi = 0; }
    mn[px] = lo; mx[px] = hi;
  }
  return [mn, mx];
}
function draw() {
  drawWave(); drawOverview(); updateTimes();
}
function tToX(t, w) { return (t - S.view.a) / (S.view.b - S.view.a) * w; }
function xToT(x, w) { return S.view.a + x / w * (S.view.b - S.view.a); }
function drawWave() {
  const c = $('wave'); const [w, h, dpr] = sizeCanvas(c); const g = c.getContext('2d');
  g.clearRect(0, 0, w, h);
  if (!S.buf) return;
  const strip = (S.cur && S.cur.subs && S.cur.subs.length) ? SUB_STRIP * dpr : 0;
  const mid = (h - strip) / 2, amp = (h - strip) / 2 - 14 * dpr;
  // ruler
  g.fillStyle = '#5f6c7b'; g.font = (10 * dpr) + 'px Consolas, monospace';
  const span = S.view.b - S.view.a; const steps = [0.001, 0.005, 0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 30, 60];
  const step = steps.find((s) => span / s <= 12) || 60;
  for (let t = Math.ceil(S.view.a / step) * step; t <= S.view.b; t += step) {
    const x = tToX(t, w); g.fillRect(x, 0, 1, 5 * dpr);
    g.fillText(step < 0.01 ? t.toFixed(3) : (step < 1 ? t.toFixed(2) : t.toFixed(0)) + 's', x + 3 * dpr, 11 * dpr);
  }
  // selection
  if (S.sel) {
    const x0 = tToX(S.sel.a, w), x1 = tToX(S.sel.b, w);
    g.fillStyle = 'rgba(229,143,176,.14)'; g.fillRect(x0, 0, x1 - x0, h);
  }
  // waveform
  const [mn, mx] = columns(S.buf, S.view.a * S.rate, S.view.b * S.rate, w);
  g.fillStyle = '#4b5a6c';
  for (let px = 0; px < w; px++) {
    const t = xToT(px + 0.5, w);
    const inSel = S.sel && t >= S.sel.a && t <= S.sel.b;
    g.fillStyle = inSel ? '#e58fb0' : '#4b5a6c';
    const y0 = mid - mx[px] * amp, y1 = mid - mn[px] * amp;
    g.fillRect(px, y0, 1, Math.max(1, y1 - y0));
  }
  g.fillStyle = '#242e3a'; g.fillRect(0, mid, w, 1);
  // fades / gain envelope over the selection
  if (S.sel) {
    const f = form(); const x0 = tToX(S.sel.a, w), x1 = tToX(S.sel.b, w);
    const len = S.sel.b - S.sel.a; let k = Math.pow(10, f.gain_db / 20);
    if (f.normalize) {
      let peak = 0; const a = Math.round(S.sel.a * S.rate), b = Math.round(S.sel.b * S.rate);
      for (let i = a; i < b; i++) { const v = Math.abs(S.buf[i]); if (v > peak) peak = v; }
      if (peak > 1e-6) k *= Math.pow(10, NORMALIZE_PEAK_DB / 20) / peak;
    }
    g.strokeStyle = 'rgba(255,255,255,.55)'; g.lineWidth = dpr; g.beginPath();
    const N = Math.max(2, Math.round(x1 - x0));
    for (let i = 0; i <= N; i++) {
      const tt = i / N * len; let e = 1;
      if (f.fade_in > 0 && tt < f.fade_in) e *= 0.5 - 0.5 * Math.cos(Math.PI * tt / f.fade_in);
      if (f.fade_out > 0 && tt > len - f.fade_out) e *= 0.5 - 0.5 * Math.cos(Math.PI * (len - tt) / f.fade_out);
      const y = mid - Math.min(1, e * Math.min(k, 4) / 4) * amp;
      if (i === 0) g.moveTo(x0 + i, y); else g.lineTo(x0 + (x1 - x0) * i / N, y);
    }
    g.stroke();
    for (const [x, which] of [[x0, 'in'], [x1, 'out']]) {
      g.fillStyle = S.handle === which ? '#e58fb0' : '#b5728b';
      g.fillRect(x - dpr, 0, 2 * dpr, h);
      g.fillRect(which === 'in' ? x : x - 8 * dpr, h - 16 * dpr, 8 * dpr, 16 * dpr);
    }
  }
  // who speaks when (cutscenes): a strip along the bottom, Fragile pink, Sam blue, the others grey
  const subs = (S.cur && S.cur.subs) || [];
  if (subs.length) {
    const bh = SUB_STRIP * dpr, by = h - bh;
    g.fillStyle = 'rgba(10,13,17,.85)'; g.fillRect(0, by, w, bh);
    g.font = (10.5 * dpr) + 'px "Segoe UI", sans-serif'; g.textBaseline = 'middle';
    for (const s of subs) {
      if (s.t1 < S.view.a || s.t0 > S.view.b) continue;
      const x0 = Math.max(0, tToX(s.t0, w)), x1 = Math.min(w, tToX(s.t1, w));
      const col = s.who === 'fragile' ? '229,143,176' : (s.who === 'sam' ? '77,163,255' : '138,151,166');
      if (s.who === 'fragile') { g.fillStyle = 'rgba(229,143,176,.06)'; g.fillRect(x0, 0, x1 - x0, by); }
      g.fillStyle = 'rgba(' + col + ',.55)'; g.fillRect(x0, by + 2 * dpr, Math.max(1, x1 - x0 - dpr), bh - 4 * dpr);
      if (x1 - x0 > 40 * dpr) {
        g.save(); g.beginPath(); g.rect(x0, by, x1 - x0 - 2 * dpr, bh); g.clip();
        g.fillStyle = '#0c1015'; g.fillText((s.who === 'fragile' ? '' : (s.who === 'sam' ? 'Sam: ' : '')) + s.text.replace(/\s+/g, ' '), x0 + 4 * dpr, by + bh / 2);
        g.restore();
      }
    }
    g.textBaseline = 'alphabetic';
  }
  // playhead
  const px = tToX(S.playhead, w);
  g.fillStyle = '#ffffff'; g.fillRect(px, 0, Math.max(1, dpr), h);
}
const SUB_STRIP = 18;   // css px
function subAt(x, y, w, h) {
  const subs = (S.cur && S.cur.subs) || [];
  if (!subs.length || y < h - SUB_STRIP) return null;
  const t = xToT(x, w);
  return subs.find((s) => t >= s.t0 && t <= s.t1) || null;
}
function drawOverview() {
  const c = $('overview'); const [w, h] = sizeCanvas(c); const g = c.getContext('2d');
  g.clearRect(0, 0, w, h);
  if (!S.buf) return;
  const [mn, mx] = columns(S.buf, 0, S.buf.length, w);
  const mid = h / 2;
  g.fillStyle = '#3a4757';
  for (let px = 0; px < w; px++) g.fillRect(px, mid - mx[px] * mid, 1, Math.max(1, (mx[px] - mn[px]) * mid));
  const d = dur();
  if (S.sel) { g.fillStyle = 'rgba(229,143,176,.35)'; g.fillRect(S.sel.a / d * w, 0, Math.max(2, (S.sel.b - S.sel.a) / d * w), h); }
  g.strokeStyle = '#e4ebf2'; g.lineWidth = 1;
  g.strokeRect(S.view.a / d * w + 0.5, 0.5, Math.max(3, (S.view.b - S.view.a) / d * w) - 1, h - 1);
  g.fillStyle = '#fff'; g.fillRect(S.playhead / d * w, 0, 1, h);
}
function updateTimes() {
  const t = $('times');
  if (!S.buf) { t.textContent = ''; return; }
  t.textContent = (S.sel ? 'In ' + fmt(S.sel.a) + '  Out ' + fmt(S.sel.b) + '  Length ' + fmt(S.sel.b - S.sel.a) + '  ·  ' : 'No selection · ') +
    'Playhead ' + fmt(S.playhead) + ' / ' + fmt(dur(), 2) + ' s';
  $('h-in').classList.toggle('on', S.handle === 'in');
  $('h-out').classList.toggle('on', S.handle === 'out');
}
function setView(a, b) {
  const d = dur(); if (!d) return;
  let span = Math.max(0.01, Math.min(d, b - a));
  a = Math.max(0, Math.min(a, d - span));
  S.view = { a, b: a + span };
  draw();
}
function zoom(f, at) {
  const span = S.view.b - S.view.a; const c = at == null ? (S.view.a + S.view.b) / 2 : at;
  const ns = span * f; const rel = (c - S.view.a) / span;
  setView(c - rel * ns, c - rel * ns + ns);
}
function clampSel() {
  if (!S.sel) return;
  const d = dur();
  S.sel.a = Math.max(0, Math.min(S.sel.a, d)); S.sel.b = Math.max(0, Math.min(S.sel.b, d));
  if (S.sel.b < S.sel.a) { const t = S.sel.a; S.sel.a = S.sel.b; S.sel.b = t; S.handle = S.handle === 'in' ? 'out' : 'in'; }
}
function selChanged() { clampSel(); draw(); updateMeter(); if (S.play && S.play.mode === 'sel' && S.play.node.loop) restartLoopSoon(); }
let loopTimer = null;
function restartLoopSoon() { clearTimeout(loopTimer); loopTimer = setTimeout(() => { if (S.play && S.play.mode === 'sel') playSelection(); }, 180); }
function nudge(d) {
  if (!S.sel) return;
  if (S.handle === 'in') S.sel.a += d; else S.sel.b += d;
  const t = S.handle === 'in' ? S.sel.a : S.sel.b;
  if (t < S.view.a || t > S.view.b) setView(t - (S.view.b - S.view.a) / 2, t + (S.view.b - S.view.a) / 2);
  selChanged();
}
function togglePlay() {
  if (!S.abuf) return;
  if (S.play && (S.play.mode === 'src' || S.play.mode === 'sel')) { stopAll(); draw(); return; }
  const from = S.playhead >= dur() - 0.01 ? 0 : S.playhead;
  playBuffer(S.abuf, { mode: 'src', offset: from });
}
function playSelection() {
  if (!S.buf || !S.sel) return;
  const x = renderClip(S.buf, S.rate, S.sel.a, S.sel.b, form());
  if (!x.length) return;
  playBuffer(bufferOf(x, S.rate), { mode: 'sel', loop: $('b-loop').checked, base: S.sel.a });
}

// mouse on the waveform
(function wireWave() {
  const c = $('wave');
  let drag = null;
  const pos = (e) => { const r = c.getBoundingClientRect(); return { x: e.clientX - r.left, w: r.width, y: e.clientY - r.top, h: r.height }; };
  c.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });
  c.addEventListener('mousedown', (e) => {
    if (!S.buf) return;
    const { x, w, y, h } = pos(e); const t = xToT(x, w);
    if (e.button === 1) {   // middle button: drag the view
      e.preventDefault();
      drag = { kind: 'pan', x0: x, a0: S.view.a, b0: S.view.b };
      c.style.cursor = 'grabbing';
      return;
    }
    if (e.button !== 0) return;
    const sub = subAt(x, y, w, h);
    if (sub) {   // a click on the speaker strip selects that line
      S.sel = { a: Math.max(0, sub.t0 - 0.1), b: Math.min(dur(), sub.t1 + 0.4) }; S.handle = 'in'; S.playhead = S.sel.a;
      const k = fragileSubs(S.cur).indexOf(sub); S.subIdx = k >= 0 ? k : S.subIdx;
      selChanged();
      return;
    }
    if (S.sel) {
      const xa = tToX(S.sel.a, w), xb = tToX(S.sel.b, w);
      if (Math.abs(x - xa) <= 7) { drag = { kind: 'in' }; S.handle = 'in'; draw(); return; }
      if (Math.abs(x - xb) <= 7) { drag = { kind: 'out' }; S.handle = 'out'; draw(); return; }
    }
    drag = { kind: 'new', x0: x, t0: t, moved: false };
  });
  window.addEventListener('mousemove', (e) => {
    if (!drag) {
      if (e.target === c && S.buf) {
        const { x, w, y, h } = pos(e);
        const near = S.sel && (Math.abs(x - tToX(S.sel.a, w)) <= 7 || Math.abs(x - tToX(S.sel.b, w)) <= 7);
        c.style.cursor = subAt(x, y, w, h) ? 'pointer' : (near ? 'ew-resize' : 'crosshair');
        c.title = (subAt(x, y, w, h) || {}).text || '';
      }
      return;
    }
    const { x, w } = pos(e);
    if (drag.kind === 'pan') {
      const d = (drag.x0 - x) / w * (drag.b0 - drag.a0);
      setView(drag.a0 + d, drag.b0 + d);
      return;
    }
    const t = Math.max(0, Math.min(dur(), xToT(x, w)));
    if (drag.kind === 'in') { S.sel.a = t; selChanged(); }
    else if (drag.kind === 'out') { S.sel.b = t; selChanged(); }
    else if (Math.abs(x - drag.x0) > 3 || drag.moved) {
      drag.moved = true;
      S.sel = { a: Math.min(drag.t0, t), b: Math.max(drag.t0, t) }; S.handle = t < drag.t0 ? 'in' : 'out';
      selChanged();
    }
  });
  window.addEventListener('mouseup', (e) => {
    if (!drag) return;
    if (drag.kind === 'pan') c.style.cursor = 'crosshair';
    if (drag.kind === 'new' && !drag.moved) { S.playhead = Math.max(0, Math.min(dur(), drag.t0)); if (S.play && S.play.mode === 'src') playBuffer(S.abuf, { mode: 'src', offset: S.playhead }); draw(); }
    drag = null;
  });
  c.addEventListener('wheel', (e) => {
    if (!S.buf) return;
    e.preventDefault();
    const { x, w } = pos(e);
    if (e.shiftKey || Math.abs(e.deltaX) > Math.abs(e.deltaY)) {
      const span = S.view.b - S.view.a; const d = (e.shiftKey ? e.deltaY : e.deltaX) / 600 * span;
      setView(S.view.a + d, S.view.b + d);
    } else zoom(e.deltaY > 0 ? 1.25 : 0.8, xToT(x, w));
  }, { passive: false });
  const ov = $('overview'); let odrag = false;
  const ovMove = (e) => {
    const r = ov.getBoundingClientRect(); const t = (e.clientX - r.left) / r.width * dur(); const span = S.view.b - S.view.a;
    setView(t - span / 2, t + span / 2);
  };
  ov.addEventListener('mousedown', (e) => { if (!S.buf || e.button > 1) return; e.preventDefault(); odrag = true; ovMove(e); });
  ov.addEventListener('auxclick', (e) => { if (e.button === 1) e.preventDefault(); });
  window.addEventListener('mousemove', (e) => { if (odrag) ovMove(e); });
  window.addEventListener('mouseup', () => { odrag = false; });
  window.addEventListener('resize', draw);
})();

// keys
document.addEventListener('keydown', (e) => {
  const tag = (e.target.tagName || '').toLowerCase();
  if ((tag === 'input' && !['range', 'checkbox'].includes(e.target.type)) || tag === 'select' || tag === 'textarea') {
    if (e.key === 'Escape') e.target.blur();
    return;
  }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') { e.preventDefault(); saveClip(); return; }
  if (e.ctrlKey || e.metaKey) return;
  const k = e.key;
  let handled = true;
  if (k === ' ') togglePlay();
  else if (k === 'Enter') playSelection();
  else if (k === 'l' || k === 'L') { $('b-loop').checked = !$('b-loop').checked; if (S.play && S.play.mode === 'sel') S.play.node.loop = $('b-loop').checked; }
  else if (k === 'i' || k === 'I') setAtPlayhead('in');
  else if (k === 'o' || k === 'O') setAtPlayhead('out');
  else if (k === '[') { S.handle = 'in'; draw(); }
  else if (k === ']') { S.handle = 'out'; draw(); }
  else if (k === 'ArrowLeft' || k === 'ArrowRight') nudge((k === 'ArrowLeft' ? -1 : 1) * (e.shiftKey ? 0.1 : (e.altKey ? 0.001 : 0.01)));
  else if (k === '+' || k === '=') zoom(0.66);
  else if (k === '-' || k === '_') zoom(1.5);
  else if (k === 'z' || k === 'Z') zoomSel();
  else if (k === 'f' || k === 'F') setView(0, dur());
  else if (k === 'n' || k === 'N') nextSub(1);
  else if (k === 'p' || k === 'P') nextSub(-1);
  else if (k === 'Home') { S.playhead = 0; draw(); }
  else if (k === 'End') { S.playhead = dur(); draw(); }
  else if (k === 's' || k === 'S') saveClip();
  else if (k === 'Escape') { $('help').classList.add('hidden'); stopAll(); draw(); }
  else handled = false;
  if (handled) e.preventDefault();
});
function setAtPlayhead(which) {
  if (!S.buf) return;
  const t = S.playhead;
  if (!S.sel) S.sel = which === 'in' ? { a: t, b: dur() } : { a: 0, b: t };
  else if (which === 'in') S.sel.a = t; else S.sel.b = t;
  S.handle = which; selChanged();
}
function zoomSel() { if (S.sel) { const pad = (S.sel.b - S.sel.a) * 0.15 + 0.02; setView(S.sel.a - pad, S.sel.b + pad); } }

// ------------------------------------------------------------------ clip form, meter, save
function samCategoryLoudness(cat) {
  const vals = [];
  for (const it of S.items) {
    if (it.speaker !== 'sam' || !itemCats(it).includes(cat)) continue;
    const m = S.user.measured[it.id]; if (m && m.loud_db > -119) vals.push(m.loud_db);
  }
  vals.sort((a, b) => a - b);
  return { n: vals.length, median: vals.length ? vals[Math.floor(vals.length / 2)] : null };
}
function updateMeter() {
  const m = $('c-meter'); m.textContent = '';
  $('c-gain-v').textContent = (parseFloat($('c-gain').value) >= 0 ? '+' : '') + parseFloat($('c-gain').value).toFixed(1) + ' dB';
  if (!S.buf || !S.sel) { m.textContent = 'Drag on the waveform to select the part to keep.'; return; }
  const st = clipStats(renderClip(S.buf, S.rate, S.sel.a, S.sel.b, form()), S.rate);
  const cat = $('c-cat').value; const sam = samCategoryLoudness(cat);
  const add = (t, cls) => { const s = el('span', cls, t); m.appendChild(s); return s; };
  add('Clip ' + fmt(st.length, 2) + ' s · peak ');
  add(dbText(st.peak_db) + ' dBFS', st.peak_db >= -0.1 ? 'bad' : '');
  add(' · loudness '); m.appendChild(el('b', null, dbText(st.loud_db) + ' dBFS'));
  if (sam.n) {
    let d = st.loud_db - sam.median; if (Math.abs(d) < 0.05) d = 0;
    add(' · Sam\'s ' + (S.catLabel[cat] || cat).toLowerCase() + ' lines ' + dbText(sam.median) + ' (' + sam.n + ' measured): ');
    add((d >= 0 ? '+' : '') + d.toFixed(1) + ' dB', Math.abs(d) <= 3 ? 'good' : (d > 0 ? 'warn' : ''));
  } else add(' · measure Sam\'s ' + (S.catLabel[cat] || cat).toLowerCase() + ' lines in Coverage to compare', 'faint');
}
for (const id of ['c-fin', 'c-fout', 'c-gain', 'c-norm', 'c-cat']) $(id).addEventListener('input', () => { draw(); updateMeter(); if (S.play && S.play.mode === 'sel' && S.play.node.loop) restartLoopSoon(); });
function setEditing(clip) {
  S.editing = clip ? clip.id : null;
  $('c-editing').textContent = clip ? 'Editing a saved clip' : '';
  $('c-save').textContent = clip ? 'Update clip' : 'Save clip';
  renderClips();
}
async function saveClip() {
  if (!S.cur || !S.sel) { alertBox('Select part of a line first.', 'error', 4000); return; }
  const f = form();
  const body = { source: S.cur.id, start: S.sel.a, end: S.sel.b, fade_in: f.fade_in, fade_out: f.fade_out,
    gain_db: f.gain_db, normalize: f.normalize, category: f.category, note: f.note };
  const old = S.editing ? S.user.clips.find((c) => c.id === S.editing) : null;
  const sits = new Set((old && old.situations) || []);
  if (S.target) sits.add(S.target.hash);   // made for the guide's current Sam situation
  body.situations = [...sits];
  if (old && old.candidate) body.candidate = old.candidate;   // saved by you: no longer a suggestion (auto is dropped)
  if (S.editing) body.id = S.editing;
  try {
    const c = await post('/api/clip', body);
    setEditing(c);
    await loadUser();
    alertBox('Saved to ' + (S.catLabel[c.category] || c.category) + '.', 'ok', 2500);
  } catch (e) { alertBox('Not saved: ' + e.message); }
}
async function editClip(c) {
  const apply = () => {
    S.sel = { a: c.start, b: c.end }; S.handle = 'in';
    $('c-fin').value = Math.round(c.fade_in * 1000); $('c-fout').value = Math.round(c.fade_out * 1000);
    $('c-gain').value = c.gain_db; $('c-norm').checked = !!c.normalize; $('c-cat').value = c.category; $('c-note').value = c.note || '';
    setEditing(c); zoomSel(); updateMeter();
  };
  if (S.cur && S.cur.id === c.source && S.buf) apply(); else await openItem(c.source, apply);
}
async function playClip(c) {
  try {
    const ab = await fetchBuffer(c.source);
    const x = renderClip(ab.getChannelData(0), ab.sampleRate, c.start, c.end, c);
    playBuffer(bufferOf(x, ab.sampleRate), { mode: 'clip' });
  } catch (e) { alertBox('Could not play: ' + e.message, 'error', 4000); }
}
async function playItem(id) {
  try { playBuffer(await fetchBuffer(id), { mode: 'line' }); } catch (e) { alertBox('Could not play: ' + e.message, 'error', 4000); }
}
async function playSequence(ids) {   // one after another, a short pause between; any other play stops it
  const tok = {};
  S.seqToken = tok;
  for (const id of ids) {
    let ab;
    try { ab = await fetchBuffer(id); } catch (e) { continue; }
    if (S.seqToken !== tok) return;
    playBuffer(ab, { mode: 'line' });
    S.seqToken = tok;
    await new Promise((r) => setTimeout(r, ab.duration * 1000 + 350));
    if (S.seqToken !== tok) return;
  }
  S.seqToken = null;
}

// ------------------------------------------------------------------ guide: Sam's situations one by one
const GUIDE_DONE = { clip: 1, any: 1, silent: 1 };
function guideSteps() {
  if (!S.catalog) return [];
  const order = Object.fromEntries(S.cats.map((c, i) => [c.key, i]));
  return S.catalog.situations.filter((s) => s.lines > 0).map((s) => ({ s, cat: samSitCat(s.hash) }))
    .sort((a, b) => (order[a.cat] - order[b.cat]) || (b.s.lines - a.s.lines) || (a.s.hash - b.s.hash));
}
function stepState(h, cat) {
  const linked = S.user.clips.filter((c) => (c.situations || []).includes(h));
  if (linked.length) {
    // only voicecut's suggestions and not kept yet: a listen is left to do
    const ok = (S.user.review || {})[String(h)] === 'ok' || linked.some((c) => !c.auto);
    return ok ? 'clip' : 'check';
  }
  const p = (S.user.plan || {})[String(h)];
  if (p === 'silent') return 'silent';
  if (p === 'any') return S.user.clips.some((c) => c.category === cat) ? 'any' : 'any-empty';
  return 'todo';
}
const STATE_TEXT = { clip: '✓ has its own clip', any: '✓ any clip of its category', 'any-empty': '! any clip of its category, but there is none yet',
  silent: '— Fragile stays silent', todo: 'to do', check: '? suggested: listen, then keep it or try another' };
async function playCandidate(c) {   // her sound as found (no gain): from its cutscene, with short fades
  try {
    const ab = await fetchBuffer(c.item);
    const x = renderClip(ab.getChannelData(0), ab.sampleRate, Math.max(0, c.t0 - 0.04), c.t1 + 0.08, { fade_in: 0.01, fade_out: 0.05 });
    playBuffer(bufferOf(x, ab.sampleRate), { mode: 'clip' });
  } catch (e) { alertBox('Could not play: ' + e.message, 'error', 4000); }
}
async function compareStep(lines, clips) {   // Sam's first line, a pause, then her clip
  if (!lines.length || !clips.length) return;
  const tok = {}; S.seqToken = tok;
  try {
    const ab = await fetchBuffer(lines[0].id);
    if (S.seqToken !== tok) return;
    playBuffer(ab, { mode: 'line' }); S.seqToken = tok;
    await new Promise((r) => setTimeout(r, ab.duration * 1000 + 450));
    if (S.seqToken !== tok) return;
    await playClip(clips[0]);
  } catch (e) { /* stopped */ }
}
async function loadSuggestions(h) {
  if (S.sugFor === h && S.sugList) return S.sugList;
  try {
    const r = await getJSON('/api/suggest?hash=' + h);
    S.sugFor = h; S.sugList = r.ready ? r.suggestions : null;
  } catch (e) { S.sugList = null; }
  return S.sugList;
}
function samLinesFor(h) { return S.items.filter((it) => it.speaker === 'sam' && (it.situations || []).includes(h)); }
function setTarget(step) {
  S.target = step ? { hash: step.s.hash, cat: step.cat, label: step.s.label } : null;
  const b = $('ed-target'); b.textContent = '';
  b.classList.toggle('hidden', !S.target);
  if (!S.target) return;
  if (step.cat !== 'other') $('c-cat').value = step.cat;
  b.appendChild(el('span', null, 'Making a clip for Sam\'s “' + step.s.label + '” (' + (S.catLabel[step.cat] || step.cat) + '). Saved clips are marked as made for it.'));
  const x = el('button', 'btn ghost small', 'Stop'); x.onclick = () => setTarget(null); b.appendChild(x);
  updateMeter();
}
function showSources(kind) {
  $('f-speaker').value = 'fragile'; $('f-kind').value = kind; $('f-cat').value = ''; $('f-voice').value = ''; $('f-search').value = '';
  S.libShown = 300; renderLibrary();
}
function guideGo(i) { S.guideIdx = i; if (S.target) { const st = guideSteps()[i]; if (st) setTarget(st); } renderGuide(); $('tab-guide').scrollTop = 0; }
function renderGuide() {
  const pane = $('tab-guide'); pane.textContent = '';
  if (!S.catalog) return;
  const steps = guideSteps();
  const states = steps.map((st) => stepState(st.s.hash, st.cat));
  const done = states.filter((x) => GUIDE_DONE[x]).length;
  $('n-guide').textContent = String(steps.length - done);
  if (S.guideIdx == null) { S.guideIdx = Math.max(0, states.findIndex((x) => !GUIDE_DONE[x])); }
  const i = Math.min(S.guideIdx, steps.length - 1);
  const top = el('div', 'guide-top');
  top.appendChild(el('div', 'cov-summary', 'Each step is one of Sam\'s situations: the sounds he makes for it in the game. Listen to him, then give Fragile a clip for it, say any clip of its category will do, or leave her silent there. ' +
    done + ' of ' + steps.length + ' done.'));
  const bar = el('div', 'cov-bar'); const fill = el('i'); fill.style.width = Math.round(100 * done / Math.max(1, steps.length)) + '%'; bar.appendChild(fill); top.appendChild(bar);
  const checks = states.filter((x) => x === 'check').length;
  const an = (S.status && S.status.analysis) || {};
  const auto = el('div', 'guide-auto');
  if (S.user.analysed) {
    const todo = states.filter((x) => x === 'todo').length;
    const b = el('button', 'btn small' + (todo ? ' primary' : ''), 'Suggest Fragile sounds for all ' + todo + ' open steps');
    b.disabled = !todo;
    b.title = 'voicecut measures (it cannot listen): her wordless sounds in the cutscenes are ranked against Sam\'s lines by length, voice, pitch and attack, and her short lines by their words. The best one is linked to each open step for you to keep or reject.';
    b.onclick = async () => {
      b.disabled = true; b.textContent = 'Linking suggestions…';
      try { const r = await post('/api/autofill', {}); await loadUser(); alertBox('Suggested ' + r.clips + ' of her sounds for ' + r.situations + ' steps. Steps marked ? are waiting for a listen.', 'ok', 6000); S.guideIdx = null; renderGuide(); } catch (e) { alertBox(e.message); }
    };
    auto.appendChild(b);
    if (checks) {
      const n = el('button', 'btn small', 'Listen to the next suggestion (' + checks + ')');
      n.onclick = () => { const k = states.findIndex((x, kk) => x === 'check' && kk !== S.guideIdx); guideGo(k >= 0 ? k : states.indexOf('check')); };
      auto.appendChild(n);
    }
  } else if (an.state === 'running') {
    auto.appendChild(el('span', 'faint', 'Measuring her sounds and Sam\'s for suggestions: ' + an.done + ' / ' + an.total + '…'));
  } else {
    auto.appendChild(el('span', 'faint', 'Suggestions come after every line is decoded (the status line above shows how far).'));
  }
  top.appendChild(auto);
  pane.appendChild(top);
  if (i < 0) return;
  const { s, cat } = steps[i];
  const state = states[i];
  const card = el('div', 'guide-card');
  const nav = el('div', 'guide-nav');
  const prev = el('button', 'btn ghost small', '◀ Back'); prev.disabled = i === 0; prev.onclick = () => guideGo(i - 1);
  nav.appendChild(prev);
  nav.appendChild(el('span', 'muted', 'Step ' + (i + 1) + ' of ' + steps.length));
  let nextTodo = states.findIndex((x, k) => k > i && !GUIDE_DONE[x]);
  if (nextTodo < 0) nextTodo = states.findIndex((x, k) => k !== i && !GUIDE_DONE[x]);
  const next = el('button', 'btn small', 'Next to do ▶'); next.disabled = nextTodo < 0; next.onclick = () => guideGo(nextTodo);
  const skip = el('button', 'btn ghost small', 'Next ▶'); skip.disabled = i >= steps.length - 1; skip.onclick = () => guideGo(i + 1);
  nav.appendChild(skip); nav.appendChild(next);
  card.appendChild(nav);
  const h = el('div', 'guide-title', s.label);
  card.appendChild(h);
  const lines = samLinesFor(s.hash);
  const meta = el('div', 'meta');
  meta.appendChild(document.createTextNode(lines.length + ' Sam line' + (lines.length === 1 ? '' : 's') + (s.spoken ? ' (' + s.spoken + ' spoken)' : ' (non-verbal)') + (s.known ? '' : ' · the game keeps only its hash') + ' · category '));
  const sel = el('select');
  const o0 = el('option', null, 'guess: ' + (S.catLabel[s.guess] || s.guess)); o0.value = ''; sel.appendChild(o0);
  for (const c of S.cats) { const o = el('option', null, c.label); o.value = c.key; sel.appendChild(o); }
  sel.value = S.user.situations[String(s.hash)] || '';
  sel.onchange = async () => { try { await post('/api/situation', { hash: s.hash, category: sel.value || null }); await loadUser(); S.guideIdx = guideSteps().findIndex((x) => x.s.hash === s.hash); renderGuide(); } catch (e) { alertBox(e.message); } };
  meta.appendChild(sel);
  card.appendChild(meta);
  // Sam's side
  const sam = el('div', 'guide-sam');
  const hs = el('div', 'guide-h'); hs.appendChild(el('span', null, 'Sam'));
  const all = el('button', 'icon sam', '▶ play them all'); all.onclick = () => playSequence(lines.map((x) => x.id)); hs.appendChild(all);
  const loud = lines.map((x) => (S.user.measured[x.id] || {}).loud_db).filter((v) => v != null && v > -119);
  if (loud.length) hs.appendChild(el('span', 'db', 'median ' + dbText(median(loud)) + ' dBFS'));
  sam.appendChild(hs);
  const shown = S.guideAll === s.hash ? lines : lines.slice(0, 8);
  shown.forEach((it, k) => {
    const r = el('div', 'cov-line');
    const p = el('button', 'icon sam', '▶'); p.onclick = () => playItem(it.id); r.appendChild(p);
    const t = el('span', 't', it.text ? it.text.replace(/\s+/g, ' ') : '(non-verbal) ' + (k + 1));
    t.title = 'Open in the editor'; t.style.cursor = 'pointer'; t.onclick = () => openItem(it.id); r.appendChild(t);
    const m = S.user.measured[it.id];
    r.appendChild(el('span', 'db', fmt(itemLength(it), 2) + ' s' + (m ? ' ' + dbText(m.loud_db) : '')));
    sam.appendChild(r);
  });
  if (lines.length > shown.length) { const b = el('button', 'btn ghost small', 'Show all ' + lines.length); b.onclick = () => { S.guideAll = s.hash; renderGuide(); }; sam.appendChild(b); }
  card.appendChild(sam);
  // Fragile's side
  const frg = el('div', 'guide-frg');
  const hf = el('div', 'guide-h'); hf.appendChild(el('span', null, 'Fragile')); hf.appendChild(el('span', 'state ' + state, STATE_TEXT[state])); frg.appendChild(hf);
  const mine = S.user.clips.filter((c) => (c.situations || []).includes(s.hash));
  if (state === 'check') {   // a suggestion waits for a listen
    const box = el('div', 'guide-check');
    box.appendChild(el('div', null, 'voicecut picked this sound of hers by measuring. Listen to Sam, then to her:'));
    const row = el('div', 'guide-acts');
    const cmp = el('button', 'btn small', '▶ Sam, then Fragile'); cmp.onclick = () => compareStep(lines, mine);
    const keep = el('button', 'btn primary small', '✓ Keep');
    keep.onclick = async () => {
      try { await post('/api/review', { hash: s.hash, ok: true }); await loadUser(); const st2 = guideSteps().map((x) => stepState(x.s.hash, x.cat)); const k = st2.findIndex((x, kk) => kk > i && x === 'check'); if (k >= 0) guideGo(k); } catch (e) { alertBox(e.message); }
    };
    const other = el('button', 'btn small', 'Try another');
    other.onclick = async () => {
      other.disabled = true;
      try { const r = await post('/api/review', { hash: s.hash, ok: false }); S.sugFor = null; await loadUser(); if (!r.next) alertBox('No other suggestion for this one: cut one yourself or leave her silent.', 'error', 5000); else { const m2 = S.user.clips.filter((c) => (c.situations || []).includes(s.hash)); compareStep(lines, m2); } } catch (e) { alertBox(e.message); }
    };
    row.appendChild(cmp); row.appendChild(keep); row.appendChild(other);
    box.appendChild(row);
    frg.appendChild(box);
  }
  for (const c of mine) {
    const r = clipRow(c);
    const un = el('button', 'icon', 'unlink'); un.title = 'Not made for this situation any more (the clip stays)';
    un.onclick = async () => { try { await post('/api/clip/link', { id: c.id, hash: s.hash, on: false }); await loadUser(); } catch (e) { alertBox(e.message); } };
    r.lastChild.appendChild(un);
    frg.appendChild(r);
  }
  const others = S.user.clips.filter((c) => !(c.situations || []).includes(s.hash))
    .sort((a, b) => (b.category === cat) - (a.category === cat));
  if (others.length) {
    const row = el('div', 'guide-row');
    const pick = el('select');
    for (const c of others) { const o = el('option', null, (S.catLabel[c.category] || c.category) + ': ' + clipLabel(c)); o.value = c.id; pick.appendChild(o); }
    const pl = el('button', 'icon play', '▶'); pl.title = 'Play it'; pl.onclick = () => { const c = others.find((x) => x.id === pick.value); if (c) playClip(c); };
    const use = el('button', 'btn ghost small', 'Use this one');
    use.onclick = async () => { try { await post('/api/clip/link', { id: pick.value, hash: s.hash, on: true }); await loadUser(); } catch (e) { alertBox(e.message); } };
    row.appendChild(pick); row.appendChild(pl); row.appendChild(use);
    frg.appendChild(row);
  }
  const acts = el('div', 'guide-acts');
  const cut = el('button', 'btn primary small', S.target && S.target.hash === s.hash ? 'Cutting a clip for this…' : 'Cut a new clip for this');
  cut.title = 'The next clip you save is made for this situation (and gets its category)';
  cut.onclick = () => { setTarget(steps[i]); renderGuide(); };
  acts.appendChild(cut);
  const plan = (S.user.plan || {})[String(s.hash)];
  const any = el('button', 'btn ghost small' + (plan === 'any' ? ' on' : ''), 'Any ' + (S.catLabel[cat] || cat).toLowerCase() + ' clip will do');
  any.disabled = cat === 'other';
  any.onclick = async () => { try { await post('/api/plan', { hash: s.hash, plan: plan === 'any' ? null : 'any' }); await loadUser(); } catch (e) { alertBox(e.message); } };
  const silent = el('button', 'btn ghost small' + (plan === 'silent' ? ' on' : ''), 'Leave Fragile silent here');
  silent.onclick = async () => { try { await post('/api/plan', { hash: s.hash, plan: plan === 'silent' ? null : 'silent' }); await loadUser(); } catch (e) { alertBox(e.message); } };
  acts.appendChild(any); acts.appendChild(silent);
  frg.appendChild(acts);
  const where = el('div', 'guide-where');
  where.appendChild(el('span', 'faint', s.spoken ? 'Sam talks here: look in Fragile\'s lines, or her lines in cutscenes. ' : 'Sam grunts or breathes here: Fragile\'s efforts and breaths are in her cutscenes. '));
  const bc = el('button', 'btn ghost small', 'Show her cutscenes'); bc.onclick = () => showSources('cutscene');
  const bl = el('button', 'btn ghost small', 'Show her lines'); bl.onclick = () => showSources('line');
  where.appendChild(bc); where.appendChild(bl);
  frg.appendChild(where);
  card.appendChild(frg);
  // ranked suggestions (measured, not heard)
  const sugBox = el('div', 'guide-sug');
  card.appendChild(sugBox);
  if (S.user.analysed) {
    loadSuggestions(s.hash).then((list) => {
      if (S.guideIdx !== i) return;
      sugBox.textContent = '';
      const hh = el('div', 'guide-h'); hh.appendChild(el('span', null, 'Suggested sounds of hers')); hh.appendChild(el('span', 'faint', 'ranked by measurement, not by ear'));
      sugBox.appendChild(hh);
      if (!list || !list.length) { sugBox.appendChild(el('div', 'faint', 'Nothing close enough: cut one yourself, or leave her silent here.')); return; }
      for (const c of list) {
        const r = el('div', 'sug');
        const p = el('button', 'icon play', '▶'); p.title = 'Play it'; p.onclick = () => playCandidate(c); r.appendChild(p);
        const it = S.byId.get(c.item);
        const label = (c.text ? '“' + c.text.replace(/\s+/g, ' ') + '”' : 'her ' + c.kind) + ' · ' + (it ? it.name : c.item) + ' @ ' + fmtTime(c.t0);
        const t = el('span', 't', label); t.title = c.why + ' (score ' + c.score + ')'; r.appendChild(t);
        r.appendChild(el('span', 'db', Math.round(c.score * 100) + '%'));
        const use = el('button', 'btn ghost small', 'Use');
        use.title = 'Make a clip of it for this step (gain matched to Sam), kept';
        use.onclick = async () => { use.disabled = true; try { await post('/api/use', { candidate: c.id, hash: s.hash }); S.sugFor = null; await loadUser(); } catch (e) { alertBox(e.message); use.disabled = false; } };
        const open = el('button', 'btn ghost small', 'Edit');
        open.title = 'Open it in the editor to trim it yourself';
        open.onclick = () => { setTarget(steps[i]); openItem(c.item, () => { S.sel = { a: Math.max(0, c.t0 - 0.04), b: c.t1 + 0.08 }; S.handle = 'in'; S.playhead = S.sel.a; zoomSel(); updateMeter(); }); };
        r.appendChild(use); r.appendChild(open);
        sugBox.appendChild(r);
      }
    });
  }
  pane.appendChild(card);
  // every step, to jump around
  const list = el('div', 'guide-list');
  const ft = el('label', 'toggle'); const cb = el('input'); cb.type = 'checkbox'; cb.checked = S.guideTodoOnly !== false;
  cb.onchange = () => { S.guideTodoOnly = cb.checked; renderGuide(); };
  ft.appendChild(cb); ft.appendChild(document.createTextNode(' Only what is left to do')); list.appendChild(ft);
  let lastCat = null;
  steps.forEach((st, k) => {
    if (S.guideTodoOnly !== false && GUIDE_DONE[states[k]] && k !== i) return;
    if (st.cat !== lastCat) { list.appendChild(el('div', 'guide-cat', S.catLabel[st.cat] || st.cat)); lastCat = st.cat; }
    const r = el('div', 'guide-step' + (k === i ? ' active' : ''));
    r.appendChild(el('span', 'state ' + states[k], GUIDE_DONE[states[k]] ? '✓' : (states[k] === 'any-empty' ? '!' : (states[k] === 'check' ? '?' : '•'))));
    r.appendChild(el('span', 'label', st.s.label));
    r.appendChild(el('span', 'db', st.s.lines + (st.s.spoken ? ' (' + st.s.spoken + ' spoken)' : '')));
    r.onclick = () => guideGo(k);
    list.appendChild(r);
  });
  pane.appendChild(list);
}

// ------------------------------------------------------------------ my clips
function clipLabel(c) {
  const it = S.byId.get(c.source);
  const base = it ? (it.kind === 'cutscene' ? it.name + ' @ ' + fmt(c.start, 2) + ' s' : itemTitle(it)) : c.source;
  return c.note ? c.note + ' — ' + base : base;
}
function clipRow(c, opts = {}) {
  const r = el('div', 'clip' + (S.editing === c.id ? ' editing' : ''));
  const p = el('button', 'icon play', '▶'); p.title = 'Play the clip'; p.onclick = (e) => { e.stopPropagation(); playClip(c); }; r.appendChild(p);
  const s = el('span', 'src', clipLabel(c)); s.title = clipLabel(c);
  if (c.auto) { const a = el('span', 'auto', 'suggested'); a.title = 'Linked by voicecut; keep it in the guide, or edit and save it to make it yours'; s.appendChild(a); }
  r.appendChild(s);
  const st = c.stats || {};
  r.appendChild(el('span', 'db', fmt(c.end - c.start, 2) + ' s ' + dbText(st.loud_db) + ' dB'));
  const tools = el('span');
  if (!opts.noEdit) {
    const ed = el('button', 'icon', '✎'); ed.title = 'Edit'; ed.onclick = () => editClip(c); tools.appendChild(ed);
    const del = el('button', 'icon', '×'); del.title = 'Delete';
    del.onclick = async () => {
      if (!confirm('Delete this clip?')) return;
      try { await post('/api/clip/delete', { id: c.id }); if (S.editing === c.id) setEditing(null); await loadUser(); } catch (e) { alertBox(e.message); }
    };
    tools.appendChild(del);
  }
  r.appendChild(tools);
  return r;
}
function renderClips() {
  const pane = $('tab-clips'); pane.textContent = '';
  if (!S.catalog) return;
  const clips = S.user.clips;
  if (!clips.length) { pane.appendChild(el('div', 'empty', 'No clips yet. Pick a Fragile line or cutscene, select a grunt or a line, choose a category and press Save clip.')); return; }
  for (const cat of S.cats) {
    const cs = clips.filter((c) => c.category === cat.key);
    if (!cs.length) continue;
    const g = el('div', 'group');
    const h = el('div', 'group-head'); h.appendChild(el('span', 'name', cat.label)); h.appendChild(el('span', 'pill', String(cs.length)));
    g.appendChild(h);
    const b = el('div', 'group-body'); for (const c of cs) b.appendChild(clipRow(c)); g.appendChild(b);
    pane.appendChild(g);
  }
}

// ------------------------------------------------------------------ coverage
function samLinesOf(cat) { return S.items.filter((it) => it.speaker === 'sam' && itemCats(it).includes(cat)); }
function renderCoverage() {
  const pane = $('tab-coverage'); pane.textContent = '';
  if (!S.catalog) return;
  const intro = el('div', 'cov-summary');
  intro.style.padding = '8px 10px 0';
  intro.textContent = 'Each category: Sam\'s situations and lines in it, and your Fragile clips. The bar fills at one clip per Sam situation (at least 2, at most 8). Click a row to compare.';
  pane.appendChild(intro);
  const head = el('div', 'cov-row headrow');
  for (const t of ['Category', 'Sam sit.', 'Sam lines', 'Your clips']) head.appendChild(el('span', t === 'Category' ? '' : 'cov-num', t));
  pane.appendChild(head);
  for (const cat of S.cats) {
    const sits = S.catalog.situations.filter((s) => samSitCat(s.hash) === cat.key);
    const lines = samLinesOf(cat.key);
    const clips = S.user.clips.filter((c) => c.category === cat.key);
    const target = Math.min(8, Math.max(2, sits.length));
    const row = el('div', 'cov-row');
    const name = el('span', null, cat.label); name.title = cat.hint; row.appendChild(name);
    row.appendChild(el('span', 'cov-num', String(sits.length)));
    row.appendChild(el('span', 'cov-num', String(lines.length)));
    const cell = el('span'); cell.style.display = 'flex'; cell.style.gap = '6px'; cell.style.alignItems = 'center';
    cell.appendChild(el('span', 'cov-num', String(clips.length)));
    const bar = el('span', 'cov-bar' + (clips.length || !sits.length ? '' : ' none')); bar.style.flex = '1';
    const fill = el('i'); fill.style.width = Math.round(100 * Math.min(1, clips.length / target)) + '%'; bar.appendChild(fill);
    if (!clips.length && sits.length) fill.style.width = '100%';
    bar.title = clips.length + ' of ' + target + ' wanted'; cell.appendChild(bar);
    row.appendChild(cell);
    row.onclick = () => { S.covOpen = S.covOpen === cat.key ? null : cat.key; renderCoverage(); };
    pane.appendChild(row);
    if (S.covOpen === cat.key) pane.appendChild(coverageDetail(cat, sits, lines, clips));
  }
}
function median(v) { if (!v.length) return null; const s = [...v].sort((a, b) => a - b); return s[Math.floor(s.length / 2)]; }
function coverageDetail(cat, sits, lines, clips) {
  const d = el('div', 'cov-detail');
  const samLoud = lines.map((i) => (S.user.measured[i.id] || {}).loud_db).filter((v) => v != null && v > -119);
  const myLoud = clips.map((c) => (c.stats || {}).loud_db).filter((v) => v != null && v > -119);
  const sm = median(samLoud), mm = median(myLoud);
  let txt = 'Sam: ' + lines.length + ' lines in ' + sits.length + ' situations';
  txt += sm != null ? ', median loudness ' + dbText(sm) + ' dBFS (' + samLoud.length + ' measured)' : ', not measured yet';
  txt += ' · You: ' + clips.length + ' clips' + (mm != null ? ', median ' + dbText(mm) + ' dBFS' : '');
  if (sm != null && mm != null) txt += ' (' + ((mm - sm) >= 0 ? '+' : '') + (mm - sm).toFixed(1) + ' dB)';
  d.appendChild(el('div', 'cov-summary', txt));
  const unmeasured = lines.filter((i) => !S.user.measured[i.id]);
  if (unmeasured.length) {
    const b = el('button', 'btn ghost small', 'Measure Sam\'s ' + unmeasured.length + ' lines (decodes them, ~' + Math.max(1, Math.round(unmeasured.length * 0.1)) + ' s)');
    b.onclick = async (e) => { e.stopPropagation(); try { await post('/api/measure', { ids: unmeasured.map((i) => i.id) }); S.wasMeasuring = true; b.disabled = true; } catch (er) { alertBox(er.message); } };
    d.appendChild(b);
  }
  const cols = el('div', 'cov-cols');
  const left = el('div'); left.appendChild(el('h3', null, 'Sam (' + lines.length + ')'));
  const shown = S.covAll === cat.key ? lines : lines.slice(0, 60);
  for (const it of shown) {
    const r = el('div', 'cov-line');
    const p = el('button', 'icon sam', '▶'); p.title = 'Play'; p.onclick = () => playItem(it.id); r.appendChild(p);
    const t = el('span', 't', itemTitle(it)); t.title = itemTitle(it); t.style.cursor = 'pointer'; t.onclick = () => openItem(it.id); r.appendChild(t);
    const m = S.user.measured[it.id]; r.appendChild(el('span', 'db', m ? dbText(m.loud_db) : ''));
    left.appendChild(r);
  }
  if (lines.length > shown.length) { const b = el('button', 'btn ghost small', 'Show all ' + lines.length); b.onclick = () => { S.covAll = cat.key; renderCoverage(); }; left.appendChild(b); }
  const right = el('div'); right.appendChild(el('h3', null, 'Your Fragile clips (' + clips.length + ')'));
  for (const c of clips) right.appendChild(clipRow(c));
  if (!clips.length) right.appendChild(el('div', 'faint', 'None yet.'));
  cols.appendChild(left); cols.appendChild(right); d.appendChild(cols);
  return d;
}

// ------------------------------------------------------------------ Sam's situations
function renderSam() {
  const list = $('sam-list'); list.textContent = '';
  if (!S.catalog) return;
  const q = $('s-search').value.trim().toLowerCase(), f = $('s-filter').value;
  const sits = S.catalog.situations.filter((s) => {
    if (q && !s.label.toLowerCase().includes(q) && !(S.catLabel[samSitCat(s.hash)] || '').toLowerCase().includes(q)) return false;
    if (f === 'unsorted' && samSitCat(s.hash) !== 'other') return false;
    if (f === 'nonverbal' && s.spoken) return false;
    if (f === 'known' && !s.known) return false;
    return s.lines > 0;
  }).sort((a, b) => (samSitCat(a.hash) === 'other') - (samSitCat(b.hash) === 'other') || b.lines - a.lines);
  for (const s of sits) {
    const r = el('div', 'sit');
    r.appendChild(el('span', 'label', s.label));
    const sel = el('select');
    const user = S.user.situations[String(s.hash)];
    const o0 = el('option', null, 'guess: ' + (S.catLabel[s.guess] || s.guess)); o0.value = ''; sel.appendChild(o0);
    for (const cat of S.cats) { const o = el('option', null, cat.label); o.value = cat.key; sel.appendChild(o); }
    sel.value = user || '';
    sel.title = 'The category Sam\'s lines in this situation count for';
    sel.onchange = async () => { try { await post('/api/situation', { hash: s.hash, category: sel.value || null }); await loadUser(); } catch (e) { alertBox(e.message); } };
    r.appendChild(sel);
    const meta = el('div', 'meta');
    meta.appendChild(document.createTextNode(s.lines + ' lines' + (s.spoken ? ' (' + s.spoken + ' spoken)' : ' (non-verbal)') + (s.priority != null ? ' · priority ' + s.priority : '') + (s.reaction ? ' · reaction' : '')));
    const lines = S.items.filter((it) => it.speaker === 'sam' && (it.situations || []).includes(s.hash));
    const pr = el('button', 'icon sam', '▶ random'); pr.title = 'Play one of these lines';
    pr.onclick = () => { if (lines.length) playItem(lines[Math.floor(Math.random() * lines.length)].id); };
    meta.appendChild(pr);
    const ex = el('button', 'icon', S.sitOpen === s.hash ? 'hide lines' : 'all lines');
    ex.onclick = () => { S.sitOpen = S.sitOpen === s.hash ? null : s.hash; renderSam(); };
    meta.appendChild(ex);
    r.appendChild(meta);
    if (S.sitOpen === s.hash) {
      const box = el('div', 'samples');
      for (const it of lines) {
        const l = el('div', 'cov-line');
        const p = el('button', 'icon sam', '▶'); p.onclick = () => playItem(it.id); l.appendChild(p);
        const t = el('span', 't', itemTitle(it)); t.style.cursor = 'pointer'; t.onclick = () => openItem(it.id); l.appendChild(t);
        l.appendChild(el('span', 'db', fmt(itemLength(it), 2) + ' s'));
        box.appendChild(l);
      }
      r.appendChild(box);
    }
    list.appendChild(r);
  }
  if (!sits.length) list.appendChild(el('div', 'empty', 'No situation matches.'));
}

// ------------------------------------------------------------------ wiring
for (const id of ['f-search', 'f-speaker', 'f-kind', 'f-cat', 'f-voice', 'f-min', 'f-max', 'f-sort']) $(id).addEventListener('input', () => { S.libShown = 300; renderLibrary(); });
for (const id of ['s-search', 's-filter']) $(id).addEventListener('input', renderSam);
for (const t of document.querySelectorAll('.tab')) {
  t.onclick = () => {
    for (const x of document.querySelectorAll('.tab')) x.classList.toggle('active', x === t);
    for (const p of ['guide', 'clips', 'coverage', 'sam']) $('tab-' + p).classList.toggle('hidden', p !== t.dataset.tab);
  };
}
$('b-play').onclick = togglePlay;
$('b-playsel').onclick = playSelection;
$('b-loop').onchange = () => { if (S.play && S.play.mode === 'sel') S.play.node.loop = $('b-loop').checked; };
$('b-zin').onclick = () => zoom(0.66);
$('b-zout').onclick = () => zoom(1.5);
$('b-zsel').onclick = zoomSel;
$('b-zfit').onclick = () => setView(0, dur());
$('h-in').onclick = () => { S.handle = 'in'; draw(); };
$('h-out').onclick = () => { S.handle = 'out'; draw(); };
$('b-setin').onclick = () => setAtPlayhead('in');
$('b-setout').onclick = () => setAtPlayhead('out');
for (const b of document.querySelectorAll('[data-nudge]')) b.onclick = () => nudge(parseFloat(b.dataset.nudge));
$('c-save').onclick = saveClip;
$('c-new').onclick = () => setEditing(null);
$('c-match').onclick = () => {
  if (!S.buf || !S.sel) { alertBox('Select part of a line first.', 'error', 4000); return; }
  const cat = $('c-cat').value; const sam = samCategoryLoudness(cat);
  if (!sam.n) { alertBox('Measure Sam\'s ' + (S.catLabel[cat] || cat).toLowerCase() + ' lines first (Coverage tab, open the category).', 'error', 6000); return; }
  const f = form(); f.gain_db = 0; f.normalize = false;
  const st = clipStats(renderClip(S.buf, S.rate, S.sel.a, S.sel.b, f), S.rate);
  if (st.loud_db <= -119) return;
  const g = Math.max(-36, Math.min(36, Math.round((sam.median - st.loud_db) * 2) / 2));
  $('c-norm').checked = false; $('c-gain').value = g;
  draw(); updateMeter();
};
$('btn-help').onclick = () => $('help').classList.remove('hidden');
$('btn-prefetch').onclick = async () => {
  const b = $('btn-prefetch');
  try { await post('/api/prefetch', { action: b.dataset.state === 'paused' ? 'resume' : 'pause' }); } catch (e) { alertBox(e.message); }
};
$('help-close').onclick = () => $('help').classList.add('hidden');
$('btn-open-export').onclick = async () => { try { await post('/api/open', { what: 'export' }); } catch (e) { alertBox(e.message); } };
$('btn-export').onclick = async () => {
  const b = $('btn-export'); b.disabled = true; b.textContent = 'Exporting…';
  try {
    const r = await post('/api/export', {});
    alertBox('Exported ' + r.clips + ' clip' + (r.clips === 1 ? '' : 's') + ' and voices.txt to ' + r.dir + (r.errors.length ? ' (' + r.errors.length + ' skipped: ' + r.errors.join('; ') + ')' : ''), r.errors.length ? 'error' : 'ok');
  } catch (e) { alertBox('Export failed: ' + e.message); }
  b.disabled = false; b.textContent = 'Export clips';
};
pollStatus();
