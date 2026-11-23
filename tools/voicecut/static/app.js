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
      box.textContent = t;
      if (!S.catalog) await loadCatalog();
      if (st.measuring || S.wasMeasuring) { S.wasMeasuring = !!st.measuring; await loadUser(); }
    }
    const busy = c.state === 'building' || Object.keys(st.decoding || {}).length || st.measuring;
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
  renderLibrary(); renderClips(); renderCoverage(); renderSam(); updateMeter();
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
  S.cur = it; S.buf = null; S.abuf = null; S.sel = null; S.playhead = 0;
  if (!then) setEditing(null);
  $('ed-title').textContent = (it.speaker === 'sam' ? 'Sam' : 'Fragile') + ' · ' + (it.kind === 'cutscene' ? 'cutscene ' + it.name : it.group);
  const src = $('ed-source'); src.textContent = '';
  if (it.kind === 'cutscene') {
    src.appendChild(el('div', null, 'Every voice in this cutscene. Fragile says:'));
    for (const t of (it.text || '').split(' / ')) src.appendChild(el('div', 'faint', '• ' + t));
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
  const mid = h / 2, amp = h / 2 - 14 * dpr;
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
  // playhead
  const px = tToX(S.playhead, w);
  g.fillStyle = '#ffffff'; g.fillRect(px, 0, Math.max(1, dpr), h);
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
  const pos = (e) => { const r = c.getBoundingClientRect(); return { x: e.clientX - r.left, w: r.width }; };
  c.addEventListener('mousedown', (e) => {
    if (!S.buf) return;
    const { x, w } = pos(e); const t = xToT(x, w);
    if (S.sel) {
      const xa = tToX(S.sel.a, w), xb = tToX(S.sel.b, w);
      if (Math.abs(x - xa) <= 7) { drag = { kind: 'in' }; S.handle = 'in'; draw(); return; }
      if (Math.abs(x - xb) <= 7) { drag = { kind: 'out' }; S.handle = 'out'; draw(); return; }
    }
    drag = { kind: 'new', x0: x, t0: t, moved: false };
  });
  window.addEventListener('mousemove', (e) => {
    if (!drag) {
      if (S.sel && e.target === c) {
        const { x, w } = pos(e); const near = Math.abs(x - tToX(S.sel.a, w)) <= 7 || Math.abs(x - tToX(S.sel.b, w)) <= 7;
        c.style.cursor = near ? 'ew-resize' : 'crosshair';
      }
      return;
    }
    const { x, w } = pos(e); const t = Math.max(0, Math.min(dur(), xToT(x, w)));
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
  ov.addEventListener('mousedown', (e) => { if (!S.buf) return; odrag = true; ovMove(e); });
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

// ------------------------------------------------------------------ my clips
function clipLabel(c) {
  const it = S.byId.get(c.source);
  const base = it ? (it.kind === 'cutscene' ? it.name + ' @ ' + fmt(c.start, 2) + ' s' : itemTitle(it)) : c.source;
  return c.note ? c.note + ' — ' + base : base;
}
function clipRow(c, opts = {}) {
  const r = el('div', 'clip' + (S.editing === c.id ? ' editing' : ''));
  const p = el('button', 'icon play', '▶'); p.title = 'Play the clip'; p.onclick = (e) => { e.stopPropagation(); playClip(c); }; r.appendChild(p);
  const s = el('span', 'src', clipLabel(c)); s.title = clipLabel(c); r.appendChild(s);
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
    for (const p of ['clips', 'coverage', 'sam']) $('tab-' + p).classList.toggle('hidden', p !== t.dataset.tab);
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
