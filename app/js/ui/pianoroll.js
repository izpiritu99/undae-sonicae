// Piano roll: canvas note editor with draw/select/slice/erase tools, velocity lane and note tools.
import { touch } from '../touch.js';
import { h, sel as mkSel, btn, icon, bus, menu, menuAt, modal, toast, clamp, uid, cssVar, noteName, isBlack, NOTES, SCALES, STEP, BAR, BEAT, PPQ } from '../util.js';
import { S, pat, chan, notesOf, commit, prefs, savePrefs, fitBars } from '../state.js';
import { engine } from '../audio/engine.js';
import { keyFromNotes } from '../audio/analysis.js';
import { actionFor } from '../keys.js';

const SNAPS = () => [[0, 'Off'], [BAR, 'Bar'], [PPQ, '1/4'], [48, '1/8'], [32, '1/8T'], [24, '1/16'], [16, '1/16T'], [12, '1/32'], [6, '1/64']];
const PITCH_HUE = [0, 30, 55, 90, 130, 165, 190, 215, 245, 275, 305, 335];
let clip = [];
// Stamps: drawing one note adds the whole chord (intervals above the note you click) or a scale stacked upwards.
const CHORDS = { Major: [0, 4, 7], Minor: [0, 3, 7], 'Major 7th': [0, 4, 7, 11], 'Minor 7th': [0, 3, 7, 10], 'Dominant 7th': [0, 4, 7, 10], 'Minor 9th': [0, 3, 7, 10, 14], 'Major 9th': [0, 4, 7, 11, 14],
  Diminished: [0, 3, 6], Augmented: [0, 4, 8], 'Suspended 2': [0, 2, 7], 'Suspended 4': [0, 5, 7], Fifth: [0, 7], 'Fifth + octave': [0, 7, 12], Octave: [0, 12], 'Add 9': [0, 4, 7, 14], 'Sixth': [0, 4, 7, 9] };
const STAMP_SCALES = { 'Major (Ionian)': [0, 2, 4, 5, 7, 9, 11], 'Minor (Aeolian)': [0, 2, 3, 5, 7, 8, 10], 'Harmonic minor': [0, 2, 3, 5, 7, 8, 11], 'Melodic minor': [0, 2, 3, 5, 7, 9, 11], Dorian: [0, 2, 3, 5, 7, 9, 10],
  Phrygian: [0, 1, 3, 5, 7, 8, 10], Lydian: [0, 2, 4, 6, 7, 9, 11], Mixolydian: [0, 2, 4, 5, 7, 9, 10], Locrian: [0, 1, 3, 5, 6, 8, 10], 'Major pentatonic': [0, 2, 4, 7, 9], 'Minor pentatonic': [0, 3, 5, 7, 10],
  Blues: [0, 3, 5, 6, 7, 10], 'Whole tone': [0, 2, 4, 6, 8, 10], 'Hungarian minor': [0, 2, 3, 6, 7, 8, 11], 'Phrygian dominant': [0, 1, 4, 5, 7, 8, 10], 'Japanese (In)': [0, 1, 5, 7, 8], Arabic: [0, 1, 4, 5, 7, 8, 11] };
const stampOf = name => CHORDS[name] || STAMP_SCALES[name] || null;
const TOOL_GLYPH = { paint: '🖌', mute: '🔇', zoom: '🔍', play: '▶' };
// Per-note controls. rel / cut / res are offsets around the channel's own setting (0 = unchanged): release time x0.25..x4,
// filter cutoff -3..+3 octaves, resonance x0.25..x4. Plug-in instruments get cut / res as CC74 / CC71 before the note.
const LANES = { v: ['Velocity', 0, 1], pan: ['Note pan', -1, 1], fine: ['Note fine pitch (cents)', -100, 100], rel: ['Note release', -1, 1], cut: ['Note filter cutoff', -1, 1], res: ['Note filter resonance', -1, 1] };
const LANE_TAG = { v: 'VEL', pan: 'PAN', fine: 'FINE', rel: 'REL', cut: 'CUT', res: 'RES' };

export default function Roll() {
  const cv = h('canvas', { tabIndex: 0 }), head = h('div', { class: 'phead' }), wrap = h('div', { class: 'cwrap' }, cv, head);
  const bar = h('div', { class: 'vbar' }), el = h('div', { class: 'view roll' }, bar, wrap), g = cv.getContext('2d');
  const V = { sx: 0, sy: -1, zx: 0.5, rh: 16, tool: 'draw', snap: STEP, lastLen: STEP, lastVel: 0.8, stamp: '', lane: 'v', slide: false };
  cv.view = V;   // for tests and debugging: the scroll / zoom state
  const sel = new Set(), keysDown = new Set(), RUL = 20, VEL = 64;
  let W = 0, H = 0, KW = 52, dpr = 1, col = {}, raf = 0, drag = null;
  const ptrs = new Map();

  const EMPTY = [];
  // Read-only access must not create empty arrays in the project (that would dirty undo snapshots).
  // With an empty rack there is nothing to edit yet: draw an empty grid instead of failing.
  const cur = () => chan() || { id: '', color: '#888', type: 'synth', root: 60, name: '(no channel)' };
  const notes = w => (!chan() ? EMPTY : w ? notesOf(pat(), cur().id) : pat().notes[cur().id] || EMPTY);
  const xOf = t => KW + t * V.zx - V.sx, tOf = x => (x - KW + V.sx) / V.zx;
  const yOf = n => RUL + (127 - n) * V.rh - V.sy, nOf = y => 127 - Math.floor((y - RUL + V.sy) / V.rh);
  const gridH = () => H - VEL - RUL;
  const snapT = (t, floor) => { const s = V.snap; return Math.max(0, s ? (floor ? Math.floor(t / s) : Math.round(t / s)) * s : Math.round(t)); };
  const snapD = d => (V.snap ? Math.round(d / V.snap) * V.snap : Math.round(d));
  const scaleSet = () => { const k = S.p.key; return !k || k.scale === 'chromatic' ? null : new Set(SCALES[k.scale].map(i => (i + k.root) % 12)); };
  const lockN = n => { const sc = prefs.scaleLock && scaleSet(); if (!sc) return n; for (const d of [0, 1, -1, 2, -2]) if (sc.has((((n + d) % 12) + 12) % 12)) return clamp(n + d, 0, 127); return n; };
  const req = () => { if (!raf) raf = requestAnimationFrame(draw); };
  const targets = () => { const ns = notes(); return sel.size ? ns.filter(n => sel.has(n.id)) : ns; };
  const audition = n => { const c = cur(); if (c) engine.preview(c, n, V.lastVel, 0.22); };

  function theme() {
    const light = document.documentElement.dataset.light === '1', k = prefs.gridContrast, a = x => Math.min(1, x * k).toFixed(3), ink = light ? '0,0,0' : '255,255,255';
    col = { bg: cssVar('--bg'), bg2: cssVar('--bg2'), bg3: cssVar('--bg3'), line: cssVar('--line'), text: cssVar('--text'), dim: cssVar('--dim'), accent: cssVar('--accent'), accent2: cssVar('--accent2'),
      rowB: `rgba(0,0,0,${light ? 0.07 : 0.2})`, bar: `rgba(${ink},${a(0.26)})`, beat: `rgba(${ink},${a(0.13)})`, sub: `rgba(${ink},${a(0.055)})`, out: `rgba(0,0,0,${light ? 0.12 : 0.32})`, light };
    req();
  }
  function clampScroll() {
    V.sx = Math.max(0, V.sx);
    V.sy = clamp(V.sy, 0, Math.max(0, 128 * V.rh - gridH()));
  }
  function focusNotes() {
    const ns = notes(), c = cur();
    const mid = ns.length ? ns.reduce((s, n) => s + n.n, 0) / ns.length : c && c.type === 'sample' ? c.root : 64;
    V.sy = (127 - mid) * V.rh - gridH() / 2; clampScroll(); req();
  }
  function resize() {
    W = wrap.clientWidth; H = wrap.clientHeight; if (!W || !H) return;
    dpr = devicePixelRatio || 1; KW = W < 560 ? 40 : 52;
    cv.width = W * dpr; cv.height = H * dpr; cv.style.width = W + 'px'; cv.style.height = H + 'px';
    if (V.sy < 0) focusNotes();
    clampScroll(); draw();
  }
  const noteCol = (n, c) => (prefs.noteColor === 'accent' ? col.accent : prefs.noteColor === 'velocity' ? `hsl(${220 - n.v * 220},75%,58%)` : prefs.noteColor === 'pitch' ? `hsl(${PITCH_HUE[n.n % 12]},65%,60%)` : c.color);

  function draw() {
    raf = 0;
    if (!W || !el.isConnected) return;
    const c = cur(), p = pat(), L = p.bars * BAR, gb = H - VEL, sc = scaleSet(), root = S.p.key ? S.p.key.root : 0, ns = notes();
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = col.bg2; g.fillRect(0, 0, W, H);
    g.save(); g.beginPath(); g.rect(KW, RUL, W - KW, gb - RUL); g.clip();
    const n0 = clamp(nOf(gb), 0, 127), n1 = clamp(nOf(RUL), 0, 127);
    for (let n = n0; n <= n1; n++) {
      const y = yOf(n);
      if (sc ? !sc.has(n % 12) : isBlack(n)) { g.fillStyle = col.rowB; g.fillRect(KW, y, W - KW, V.rh); }
      else if (sc && n % 12 === root) { g.globalAlpha = 0.1; g.fillStyle = col.accent; g.fillRect(KW, y, W - KW, V.rh); g.globalAlpha = 1; }
      if (n % 12 === 0) { g.fillStyle = col.beat; g.fillRect(KW, y + V.rh - 1, W - KW, 1); }
    }
    let gs = V.snap || STEP; while (gs * V.zx < 7) gs *= 2;
    for (let t = Math.max(0, Math.floor(tOf(KW) / gs) * gs), t1 = tOf(W); t <= t1; t += gs) {
      g.fillStyle = t % BAR === 0 ? col.bar : t % BEAT === 0 ? col.beat : col.sub; g.fillRect(Math.round(xOf(t)), RUL, 1, gb - RUL);
    }
    const xe = xOf(L); if (xe < W) { g.fillStyle = col.out; g.fillRect(Math.max(KW, xe), RUL, W, gb); }
    if (prefs.ghost) {
      g.globalAlpha = 0.22;
      for (const oc of S.p.channels) if (oc.id !== c.id) for (const n of p.notes[oc.id] || []) { g.fillStyle = oc.color; g.fillRect(xOf(n.t), yOf(n.n) + 3, Math.max(2, n.d * V.zx - 1), V.rh - 6); }
      g.globalAlpha = 1;
    }
    for (const n of ns) {
      const x = xOf(n.t), w = Math.max(3, n.d * V.zx - 1), y = yOf(n.n);
      if (x > W || x + w < KW || y > gb || y + V.rh < RUL) continue;
      const on = sel.has(n.id);
      g.globalAlpha = n.mute ? 0.18 : 0.45 + 0.55 * n.v; g.fillStyle = n.mute ? col.dim : noteCol(n, c);
      g.beginPath(); g.roundRect ? g.roundRect(x, y + 1, w, V.rh - 2, Math.min(4, prefs.radius / 3)) : g.rect(x, y + 1, w, V.rh - 2); g.fill();
      g.globalAlpha = 1;
      if (on) { g.strokeStyle = col.text; g.lineWidth = 1.5; g.stroke(); }
      else if (n.slide) { g.setLineDash([3, 2]); g.strokeStyle = col.text; g.lineWidth = 1; g.stroke(); g.setLineDash([]); }
      g.fillStyle = 'rgba(0,0,0,.35)'; g.fillRect(x + w - 3, y + 3, 1.5, V.rh - 6);
      if (n.slide) { const m = y + V.rh / 2, k = Math.min(5, V.rh / 2 - 2); g.fillStyle = 'rgba(0,0,0,.75)'; g.beginPath(); g.moveTo(x + 2, m - k); g.lineTo(x + 2 + k * 1.4, m); g.lineTo(x + 2, m + k); g.fill(); }
      if (w > 30 && V.rh >= 13) { g.fillStyle = 'rgba(0,0,0,.7)'; g.font = '10px system-ui,sans-serif'; g.textBaseline = 'middle'; g.fillText(noteName(n.n), x + (n.slide ? 12 : 4), y + V.rh / 2 + 0.5); }
    }
    if (drag && drag.type === 'box') {
      g.strokeStyle = col.accent2; g.fillStyle = col.accent2; g.globalAlpha = 0.15;
      const bx = Math.min(drag.x0, drag.x1), by = Math.min(drag.y0, drag.y1), bw = Math.abs(drag.x1 - drag.x0), bh = Math.abs(drag.y1 - drag.y0);
      g.fillRect(bx, by, bw, bh); g.globalAlpha = 1; g.strokeRect(bx + 0.5, by + 0.5, bw, bh);
    }
    g.restore();
    // control lane: velocity, note pan or note fine pitch (click the label on the left to switch)
    const [, lmin, lmax] = LANES[V.lane], bi = lmin < 0, laneY = v => H - 4 - (v - lmin) / (lmax - lmin) * (VEL - 10);
    g.fillStyle = col.bg; g.fillRect(KW, gb, W - KW, VEL); g.fillStyle = col.line; g.fillRect(0, gb, W, 1);
    if (bi) { g.fillStyle = col.sub; g.fillRect(KW, laneY(0), W - KW, 1); }
    for (const n of ns) {
      const x = xOf(n.t); if (x < KW - 2 || x > W) continue;
      const val = V.lane === 'v' ? n.v : n[V.lane] || 0, y = laneY(val), y0 = bi ? laneY(0) : H - 4;
      g.fillStyle = sel.has(n.id) ? col.accent2 : noteCol(n, c); g.fillRect(x, Math.min(y, y0), 3, Math.max(1, Math.abs(y0 - y))); g.beginPath(); g.arc(x + 1.5, y, 3, 0, 7); g.fill();
    }
    g.fillStyle = col.bg2; g.fillRect(0, gb + 1, KW, VEL); g.fillStyle = col.dim; g.font = '10px system-ui,sans-serif'; g.textBaseline = 'middle'; g.fillText(LANE_TAG[V.lane] + ' ▾', 6, gb + VEL / 2);
    // ruler
    g.fillStyle = col.bg; g.fillRect(0, 0, W, RUL);
    if (p.loop && p.loop.b > p.loop.a) { g.fillStyle = col.accent2; g.globalAlpha = p.loop.on ? 0.45 : 0.15; g.fillRect(Math.max(KW, xOf(p.loop.a)), 0, xOf(p.loop.b) - Math.max(KW, xOf(p.loop.a)), RUL); g.globalAlpha = 1; }
    g.fillStyle = col.dim; g.textBaseline = 'middle';
    for (let b = Math.max(0, Math.floor(tOf(KW) / BAR)); xOf(b * BAR) < W; b++) { const x = xOf(b * BAR); if (x >= KW - 1) { g.fillRect(x, RUL - 6, 1, 6); g.fillText(String(b + 1), x + 4, RUL / 2); } }
    g.fillStyle = col.bg; g.fillRect(0, 0, KW, RUL);
    // keys
    g.save(); g.beginPath(); g.rect(0, RUL, KW, gb - RUL); g.clip();
    for (let n = n0; n <= n1; n++) {
      const y = yOf(n), bk = isBlack(n);
      g.fillStyle = keysDown.has(n) ? col.accent : bk ? '#23212a' : '#ebe7e1'; g.fillRect(0, y, bk ? KW * 0.64 : KW - 1, V.rh - (bk ? 0 : 1));
      if (bk) { g.fillStyle = '#ebe7e1'; g.fillRect(KW * 0.64, y, KW * 0.36 - 1, V.rh); g.fillStyle = 'rgba(0,0,0,.25)'; g.fillRect(KW * 0.64, y + V.rh / 2, KW * 0.36 - 1, 1); }
      if (n % 12 === 0 || (c.type === 'sample' && n === c.root)) { g.fillStyle = n === c.root && c.type === 'sample' ? '#c2410c' : '#55504a'; g.font = '10px system-ui,sans-serif'; g.textBaseline = 'middle'; g.textAlign = 'right'; g.fillText(noteName(n), KW - 5, y + V.rh / 2); g.textAlign = 'left'; }
    }
    g.restore();
    g.fillStyle = col.line; g.fillRect(KW - 1, 0, 1, H);
  }

  const noteAt = (x, y) => { if (x < KW || y < RUL || y > H - VEL) return null; const ns = notes(), t = tOf(x), n = nOf(y); for (let i = ns.length - 1; i >= 0; i--) if (ns[i].n === n && t >= ns[i].t && t < ns[i].t + Math.max(ns[i].d, 3 / V.zx)) return ns[i]; return null; };
  function done() {
    const p = pat(), ns = notes(), end = ns.reduce((m, n) => Math.max(m, n.t + n.d), 0);
    for (const id of sel) if (!ns.some(n => n.id === id)) sel.delete(id);
    if (end > p.bars * BAR && p.bars < 64) { p.bars = fitBars(end); commit(); } else commit('notes');
    req();
  }
  const remove = pred => { const p = pat(), id = cur().id; p.notes[id] = notes().filter(n => !pred(n)); };
  function eraseAt(x, y) { const hit = noteAt(x, y); if (hit) { remove(n => n === hit); sel.delete(hit.id); req(); } }
  function sliceAt(x, y) {
    const hit = noteAt(x, y); if (!hit) return;
    const t = snapT(tOf(x)); if (t <= hit.t || t >= hit.t + hit.d) return;
    notes(true).push({ ...hit, id: uid(), t, d: hit.t + hit.d - t }); hit.d = t - hit.t; req();
  }
  function setVel(x, y) {   // draw in the control lane: velocity, pan or fine pitch of the notes under the pointer
    const [, lmin, lmax] = LANES[V.lane], v = clamp(lmin + (H - 4 - y) / (VEL - 10) * (lmax - lmin), V.lane === 'v' ? 0.02 : lmin, lmax);
    if (V.lane === 'v') V.lastVel = v;
    const val = V.lane === 'fine' ? Math.round(v) : V.lane !== 'v' && Math.abs(v) < 0.04 ? 0 : v;
    for (const n of targets()) if (Math.abs(xOf(n.t) + 1.5 - x) < 7) { if (V.lane === 'v') n.v = val; else if (val) n[V.lane] = val; else delete n[V.lane]; }
    req();
  }
  function beginMove(type, x, y, ref) { drag = { type, x, y, ref, orig: new Map(notes().filter(n => sel.has(n.id)).map(n => [n.id, { t: n.t, d: n.d, n: n.n }])), lastN: ref.n }; }
  function cancelDrag() { if (drag && drag.created) { remove(n => n.id === drag.ref.id); sel.clear(); } if (drag && drag.type === 'key') engine.noteOff(drag.n); drag = null; }
  const local = e => { const r = cv.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };

  cv.addEventListener('pointerdown', e => {
    cv.focus({ preventScroll: true });
    if (!chan()) { toast('Add a channel in the Rack first'); return; }
    const [x, y] = local(e);
    ptrs.set(e.pointerId, [x, y]); try { cv.setPointerCapture(e.pointerId); } catch { /* synthetic pointer */ }
    const pan = () => { drag = { type: 'pan', x, y, sx: V.sx, sy: V.sy }; };
    if (ptrs.size === 2) {   // two fingers: pinch to zoom (spread sideways: time, up and down: row height) and drag to pan
      cancelDrag(); const [a, b] = [...ptrs.values()], cx = (a[0] + b[0]) / 2, cy = (a[1] + b[1]) / 2;
      drag = { type: 'pan', pinch: true, x: cx, y: cy, sx: V.sx, sy: V.sy, zx0: V.zx, rh0: V.rh, w0: Math.max(40, Math.abs(a[0] - b[0])), h0: Math.max(40, Math.abs(a[1] - b[1])), t0: tOf(cx), r0: (cy - RUL + V.sy) / V.rh };
      return;
    }
    if (e.button === 1 || V.tool === 'pan') { e.preventDefault(); pan(); return; }
    if (y < RUL) { if (x >= KW && e.button === 0) drag = { type: 'loop', t0: snapT(tOf(x)) }; return; }   // drag on the ruler: loop range
    if (y > H - VEL) {
      if (x >= KW) { drag = { type: 'vel' }; setVel(x, y); }
      else menu(e.clientX, e.clientY, [{ head: 'Control lane' }, ...Object.entries(LANES).map(([k, [l]]) => ({ label: l, on: V.lane === k, fn: () => { V.lane = k; req(); } }))]);
      return;
    }
    if (x < KW) { const n = clamp(nOf(y), 0, 127); engine.noteOn(n, 0.8); drag = { type: 'key', n }; return; }
    const hit = noteAt(x, y);
    if (V.tool === 'zoom') { zoom(e.button === 2 ? 1 / 1.5 : 1.5, x); return; }
    if (V.tool === 'play' && e.button === 0) { if (engine.playing) engine.stop(); if (S.mode !== 'pat') { S.mode = 'pat'; bus.emit('sel'); } engine.play(Math.max(0, snapT(tOf(x), true))); return; }
    if (V.tool === 'mute' && e.button === 0) {
      const v = hit ? !hit.mute : true, done = new Set();
      drag = { type: 'mute', hit: (hx, hy) => { const n = noteAt(hx, hy); if (n && !done.has(n.id)) { done.add(n.id); if (v) n.mute = true; else delete n.mute; } } };
      drag.hit(x, y); req(); return;
    }
    if (V.tool === 'paint' && e.button === 0) {   // drag along a row: notes of the last length, one after another on the grid
      const n0 = lockN(clamp(nOf(y), 0, 127)), t0 = snapT(tOf(x), true), len = V.lastLen, made = new Set();
      drag = { type: 'paint', hit: hx => { const t = snapT(tOf(hx), true); for (let s = t0; s <= t; s += len) if (!made.has(s)) { made.add(s); notes(true).push({ id: uid(), t: s, d: len, n: n0, v: V.lastVel, ...(V.slide ? { slide: true } : {}) }); } } };
      drag.hit(x); audition(n0); req(); return;
    }
    if (e.button === 2 || V.tool === 'erase') { drag = { type: 'erase' }; eraseAt(x, y); return; }
    if (V.tool === 'slice') { drag = { type: 'slice' }; sliceAt(x, y); return; }
    // Ctrl+drag box-selects with any tool (Shift adds to the selection).
    if ((e.ctrlKey || e.metaKey) && e.button === 0) { if (!e.shiftKey) sel.clear(); drag = { type: 'box', x0: x, y0: y, x1: x, y1: y, base: new Set(sel) }; req(); return; }
    if (hit) {
      if (e.shiftKey) { sel.has(hit.id) ? sel.delete(hit.id) : sel.add(hit.id); req(); return; }
      if (!sel.has(hit.id)) { sel.clear(); sel.add(hit.id); }
      const edge = x > xOf(hit.t + hit.d) - Math.min(9, hit.d * V.zx * 0.35);
      beginMove(edge ? 'size' : 'move', x, y, hit); V.lastLen = hit.d; V.lastVel = hit.v;
      if (!edge) audition(hit.n);
    } else if (V.tool === 'draw') {
      const note = { id: uid(), t: snapT(tOf(x), true), d: V.lastLen, n: lockN(clamp(nOf(y), 0, 127)), v: V.lastVel, ...(V.slide ? { slide: true } : {}) };
      notes(true).push(note); sel.clear(); sel.add(note.id);
      const st = stampOf(V.stamp);   // chord / scale stamp: the other notes come along, selected so they move together
      if (st) for (const iv of st.slice(1)) { const n = note.n + iv; if (n > 127) break; const z = { ...note, id: uid(), n }; notes(true).push(z); sel.add(z.id); }
      beginMove('move', x, y, note); drag.created = !st; audition(note.n);
    } else { if (!e.shiftKey) sel.clear(); drag = { type: 'box', x0: x, y0: y, x1: x, y1: y, base: new Set(sel) }; }
    req();
  });
  cv.addEventListener('pointermove', e => {
    const [x, y] = local(e);
    if (!drag) { const hit = noteAt(x, y); cv.style.cursor = V.tool === 'pan' ? 'grab' : hit && x > xOf(hit.t + hit.d) - 9 ? 'ew-resize' : hit ? 'move' : x < KW ? 'pointer' : 'crosshair'; return; }
    if (ptrs.has(e.pointerId)) ptrs.set(e.pointerId, [x, y]);
    const d = drag;
    if (d.type === 'pan' && d.pinch) {
      if (ptrs.size < 2) return;
      const [a, b] = [...ptrs.values()], cx = (a[0] + b[0]) / 2, cy = (a[1] + b[1]) / 2, w = Math.abs(a[0] - b[0]), hh = Math.abs(a[1] - b[1]);
      if (d.w0 > 60 || w > 60) V.zx = clamp(d.zx0 * Math.max(40, w) / d.w0, 0.04, 6);
      if (d.h0 > 60 || hh > 60) V.rh = clamp(Math.round(d.rh0 * Math.max(40, hh) / d.h0), 8, 40);
      V.sx = d.t0 * V.zx - (cx - KW); V.sy = d.r0 * V.rh - (cy - RUL); clampScroll();
    } else if (d.type === 'pan') { V.sx = d.sx - (x - d.x); V.sy = d.sy - (y - d.y); clampScroll(); }
    else if (d.type === 'move') {
      let dt = snapD((x - d.x) / V.zx); const dn = -Math.round((y - d.y) / V.rh);
      for (const o of d.orig.values()) dt = Math.max(dt, -o.t);
      for (const n of notes()) { const o = d.orig.get(n.id); if (o) { n.t = o.t + dt; n.n = lockN(clamp(o.n + dn, 0, 127)); } }
      if (d.ref.n !== d.lastN) { d.lastN = d.ref.n; audition(d.ref.n); }
      d.moved = true;
    } else if (d.type === 'size') {
      const dd = snapD((x - d.x) / V.zx), min = V.snap || 6;
      for (const n of notes()) { const o = d.orig.get(n.id); if (o) n.d = Math.max(Math.min(min, o.d), o.d + dd); }
    } else if (d.type === 'box') {
      d.x1 = x; d.y1 = y; sel.clear(); d.base.forEach(i => sel.add(i));
      const ta = tOf(Math.min(d.x0, x)), tb = tOf(Math.max(d.x0, x)), na = nOf(Math.max(d.y0, y)), nb = nOf(Math.min(d.y0, y));
      for (const n of notes()) if (n.t < tb && n.t + n.d > ta && n.n >= na && n.n <= nb) sel.add(n.id);
    } else if (d.type === 'erase') eraseAt(x, y);
    else if (d.type === 'slice') sliceAt(x, y);
    else if (d.type === 'vel') setVel(x, y);
    else if (d.type === 'mute') d.hit(x, y);
    else if (d.type === 'paint') d.hit(x);
    else if (d.type === 'loop') { const t1 = snapT(tOf(x)), p = pat(); p.loop = { on: true, a: Math.min(d.t0, t1), b: Math.max(d.t0, t1) }; d.moved = true; }
    else if (d.type === 'key') { const n = clamp(nOf(y), 0, 127); if (n !== d.n) { engine.noteOff(d.n); engine.noteOn(n, 0.8); d.n = n; } }
    req();
  });
  const up = e => {
    ptrs.delete(e.pointerId);
    const d = drag; if (!d) return; drag = null;
    if (d.type === 'key') engine.noteOff(d.n);
    else if (d.type === 'size') { V.lastLen = d.ref.d; done(); }
    else if (['move', 'erase', 'slice', 'vel', 'mute', 'paint'].includes(d.type)) done();
    else if (d.type === 'loop') { const p = pat(); if (!d.moved || !p.loop || p.loop.b - p.loop.a < 3) { if (p.loop) p.loop.on = false; } commit(''); toolbar(); }
    req();
  };
  cv.addEventListener('pointerup', up); cv.addEventListener('pointercancel', up);
  // The right mouse button erases notes (pointerdown above). A long-press (touch) opens a menu instead: the note's
  // actions, or paste / select / tools on empty grid.
  cv.addEventListener('contextmenu', e => {
    e.preventDefault(); if (touch.last !== 'touch') return;
    const [x, y] = local(e); if (y < RUL || y > H - VEL || x < KW) return;
    cancelDrag(); ptrs.clear();
    const hit = noteAt(x, y);
    if (hit) {
      if (!sel.has(hit.id)) { sel.clear(); sel.add(hit.id); } req();
      const n = sel.size;
      menu(e.clientX, e.clientY, [{ head: n > 1 ? `${n} notes` : noteName(hit.n) + ' · ' + Math.round(hit.v * 100) + '%' },
        { label: 'Delete', fn: () => run('del') }, { label: 'Duplicate', fn: () => run('dup') }, { label: 'Copy', fn: () => T.copy() },
        { label: 'Velocity', sub: [1, 0.85, 0.7, 0.55, 0.4, 0.25].map(v => ({ label: Math.round(v * 100) + '%', fn: () => { targets().forEach(z => { z.v = v; }); done(); } })) },
        { label: hit.mute ? 'Unmute' : 'Mute', fn: () => { const m = !hit.mute; targets().forEach(z => { if (m) z.mute = true; else delete z.mute; }); done(); } },
        { label: hit.slide ? 'Make normal note' : 'Make slide note', fn: slideSel },
        { label: 'Add to selection by dragging', fn: () => { V.tool = 'select'; toolbar(); toast('Select tool: drag a box around notes'); } }, '-',
        { label: 'Deselect', fn: () => { sel.clear(); req(); } }]);
    } else {
      menu(e.clientX, e.clientY, [{ head: 'Piano Roll' }, { label: 'Paste', fn: () => run('paste') }, { label: 'Select all', fn: () => { notes().forEach(n => sel.add(n.id)); req(); } },
        { label: 'Deselect', disabled: !sel.size, fn: () => { sel.clear(); req(); } }, { label: 'Tools…', fn: () => toolsMenu({ currentTarget: { getBoundingClientRect: () => ({ left: e.clientX, bottom: e.clientY }) } }) }]);
    }
  });
  // Double-click a note: set its velocity (and that of every selected note, when it is one of them).
  cv.addEventListener('dblclick', e => {
    const [x, y] = local(e), hit = noteAt(x, y); if (!hit) return;
    const group = sel.has(hit.id) ? notes().filter(n => sel.has(n.id)) : [hit];
    menu(e.clientX, e.clientY, [{ head: `Velocity · ${Math.round(hit.v * 100)}%${group.length > 1 ? ` · ${group.length} notes` : ''}` }, ...[1, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1].map(v => ({
      label: Math.round(v * 100) + '%', on: Math.abs(hit.v - v) < 0.05,
      fn: () => { group.forEach(n => { n.v = v; }); commit('notes'); req(); if (!engine.playing) engine.preview(chan(), hit.n, v, 0.2); },
    }))]);
  });
  cv.addEventListener('wheel', e => {
    e.preventDefault();
    const [x] = local(e);
    if (e.ctrlKey || e.metaKey) zoom(e.deltaY < 0 ? 1.18 : 0.85, x);
    else if (e.altKey) { const mid = nOf(H / 2); V.rh = clamp(V.rh + (e.deltaY < 0 ? 2 : -2), 8, 32); V.sy = (127 - mid) * V.rh - gridH() / 2; }
    else if (e.shiftKey) V.sx += e.deltaY || e.deltaX;
    else { V.sy += e.deltaY; V.sx += e.deltaX; }
    clampScroll(); req();
  }, { passive: false });
  function zoom(k, x = KW + (W - KW) / 2) { const t = tOf(x); V.zx = clamp(V.zx * k, 0.04, 6); V.sx = t * V.zx - (x - KW); clampScroll(); req(); }

  // --- note tools -------------------------------------------------------
  const chords = ns => { const out = []; let cur = null; for (const n of [...ns].sort((a, b) => a.t - b.t || a.n - b.n)) { if (cur && n.t < cur.end) { cur.notes.push(n); cur.end = Math.max(cur.end, n.t + n.d); } else out.push(cur = { t: n.t, end: n.t + n.d, notes: [n] }); } return out; };
  const T = {
    quantize(strength = 1, ends) {
      const s = V.snap || STEP;
      for (const n of targets()) { const q = Math.round(n.t / s) * s; n.t = Math.round(n.t + (q - n.t) * strength); if (ends) n.d = Math.max(s, Math.round(n.d / s) * s); }
    },
    legato() { const ts = targets(), starts = [...new Set(ts.map(n => n.t))].sort((a, b) => a - b); for (const n of ts) { const nx = starts.find(t => t > n.t); if (nx != null) n.d = nx - n.t; } },
    chop() { const s = V.snap || STEP, ts = targets(), ns = notes(true); for (const n of ts) { if (n.d <= s) continue; const end = n.t + n.d; let t = (Math.floor(n.t / s) + 1) * s; n.d = t - n.t; for (; t < end; t += s) { const z = { ...n, id: uid(), t, d: Math.min(s, end - t) }; ns.push(z); sel.size && sel.add(z.id); } } },
    transpose(d) { for (const n of targets()) n.n = clamp(n.n + d, 0, 127); },
    scaleLen(k) { for (const n of targets()) n.d = Math.max(3, Math.round(n.d * k)); },
    humanize() { for (const n of targets()) { n.t = Math.max(0, n.t + Math.round((Math.random() - 0.5) * 6)); n.v = clamp(n.v * (0.88 + Math.random() * 0.24), 0.05, 1); } },
    strum(step = 4) { for (const c of chords(targets())) c.notes.sort((a, b) => a.n - b.n).forEach((n, i) => { const sh = Math.min(i * step, n.d - 3); n.t += sh; n.d -= sh; }); },
    reverse() { const ts = targets(); if (!ts.length) return; const a = Math.min(...ts.map(n => n.t)), b = Math.max(...ts.map(n => n.t + n.d)); for (const n of ts) n.t = a + b - (n.t + n.d); },
    arp({ mode, rate, octs, gate }) {
      const ts = targets(), ids = new Set(ts.map(n => n.id)), made = [];
      for (const c of chords(ts)) {
        let pcs = [...new Set(c.notes.map(n => n.n))].sort((a, b) => a - b); const v = c.notes.reduce((s, n) => s + n.v, 0) / c.notes.length;
        pcs = Array.from({ length: octs }, (_, o) => pcs.map(n => n + 12 * o)).flat().filter(n => n <= 127);
        const seq = mode === 'down' ? [...pcs].reverse() : mode === 'updown' && pcs.length > 2 ? [...pcs, ...pcs.slice(1, -1).reverse()] : pcs;
        for (let t = c.t, i = 0; t < c.end; t += rate, i++) made.push({ id: uid(), t, d: Math.max(3, Math.round(Math.min(rate, c.end - t) * gate)), n: mode === 'random' ? pcs[Math.floor(Math.random() * pcs.length)] : seq[i % seq.length], v });
      }
      remove(n => ids.has(n.id)); notes(true).push(...made); sel.clear(); made.forEach(n => sel.add(n.id));
    },
    glue() {   // join notes of the same pitch that touch or overlap into one
      const ts = [...targets()].sort((a, b) => a.n - b.n || a.t - b.t), gone = new Set();
      for (let i = 0; i < ts.length; i++) { const a = ts[i]; if (gone.has(a.id)) continue; for (let j = i + 1; j < ts.length && ts[j].n === a.n; j++) { const b = ts[j]; if (b.t <= a.t + a.d + 1) { a.d = Math.max(a.d, b.t + b.d - a.t); gone.add(b.id); } } }
      remove(n => gone.has(n.id)); gone.forEach(id => sel.delete(id));
    },
    flam(gap = 6) { const ns = notes(true), made = []; for (const n of targets()) if (n.t >= gap) made.push({ ...n, id: uid(), t: n.t - gap, d: Math.min(gap, n.d), v: n.v * 0.6 }); ns.push(...made); },
    limit(lo, hi) { for (const n of targets()) { while (n.n < lo) n.n += 12; while (n.n > hi) n.n -= 12; n.n = clamp(n.n, 0, 127); } },
    flip() { const ts = targets(); if (!ts.length) return; const a = Math.min(...ts.map(n => n.n)), b = Math.max(...ts.map(n => n.n)); for (const n of ts) n.n = a + b - n.n; },
    randomize(o) { for (const n of targets()) { if (o.vel) n.v = clamp(n.v + (Math.random() - 0.5) * o.vel, 0.05, 1); if (o.time) n.t = Math.max(0, n.t + Math.round((Math.random() - 0.5) * o.time)); if (o.pan) n.pan = clamp((Math.random() - 0.5) * 2 * o.pan, -1, 1); } },
    levels(lo, hi) { const ts = [...targets()].sort((a, b) => a.t - b.t); if (!ts.length) return; const a = ts[0].t, b = ts[ts.length - 1].t || 1; for (const n of ts) n.v = clamp(lo + (hi - lo) * (b > a ? (n.t - a) / (b - a) : 1), 0.02, 1); },
    lfo(o) { for (const n of targets()) { const s = Math.sin(n.t / (o.period * BAR) * Math.PI * 2); if (o.lane === 'v') n.v = clamp(o.center + s * o.depth, 0.02, 1); else if (o.lane === 'fine') n.fine = Math.round(s * o.depth * 100); else n[o.lane] = clamp(s * o.depth, -1, 1); } },
    progression(o) {   // chords of the current key, one per `len` ticks, from the play position (or the start)
      const k = S.p.key || { root: 0, scale: 'major' }, sc = SCALES[k.scale && k.scale !== 'chromatic' ? k.scale : 'major'] || SCALES.major, ns = notes(true), base = (o.oct + 1) * 12 + (k.root || 0);
      sel.clear();
      o.degrees.forEach((dg, i) => { for (const step of [0, 2, 4]) { const idx = dg + step, n = base + sc[idx % sc.length] + 12 * Math.floor(idx / sc.length); const z = { id: uid(), t: o.at + i * o.len, d: o.len, n, v: 0.75 }; ns.push(z); sel.add(z.id); } });
    },
    del() { const ids = new Set(targets().map(n => n.id)); if (!sel.size) return; remove(n => ids.has(n.id)); sel.clear(); },
    copy() { const ts = targets(); if (!ts.length) return; const t0 = Math.min(...ts.map(n => n.t)); clip = ts.map(n => ({ ...n, t: n.t - t0 })); toast(`Copied ${clip.length} note${clip.length > 1 ? 's' : ''}`); },
    paste(at) {
      if (!clip.length) return;
      const cur = notes().filter(n => sel.has(n.id)), t0 = at ?? (cur.length ? Math.max(...cur.map(n => n.t + n.d)) : snapT(tOf(KW), true));
      sel.clear(); for (const n of clip) { const z = { ...n, id: uid(), t: t0 + n.t }; notes(true).push(z); sel.add(z.id); }
    },
    dup() { const ts = notes().filter(n => sel.has(n.id)); if (!ts.length) return; const a = Math.min(...ts.map(n => n.t)), b = Math.max(...ts.map(n => n.t + n.d)), save = clip; clip = ts.map(n => ({ ...n, t: n.t - a })); T.paste(a + (snapD(b - a) || b - a)); clip = save; },
  };
  const run = (fn, ...a) => { T[fn](...a); done(); };
  function arpDialog() {
    const o = { mode: 'up', rate: STEP, octs: 1, gate: 0.8 };
    modal('Arpeggiate', h('div', { class: 'form' },
      h('label', null, 'Direction', mkSel([['up', 'Up'], ['down', 'Down'], ['updown', 'Up & down'], ['random', 'Random']], o.mode, v => { o.mode = v; })),
      h('label', null, 'Rate', mkSel(SNAPS().slice(2), o.rate, v => { o.rate = +v; })),
      h('label', null, 'Octaves', mkSel([1, 2, 3, 4], 1, v => { o.octs = +v; })),
      h('label', null, 'Gate', mkSel([[0.4, '40%'], [0.6, '60%'], [0.8, '80%'], [1, '100%']], 0.8, v => { o.gate = +v; })),
      h('small', null, 'Applies to the selection (or every note). Overlapping notes are treated as one chord.')),
      [{ label: 'Cancel' }, { label: 'Arpeggiate', primary: true, fn: () => run('arp', o) }]);
  }
  // Small dialogs for the tools that take settings.
  const dlg = (title, fields, go, note) => { const o = {}; modal(title, h('div', { class: 'form' }, ...fields.map(([k, label, opts, def]) => { o[k] = def; return h('label', null, label, mkSel(opts, def, v => { o[k] = isNaN(+v) ? v : +v; })); }), note && h('small', null, note)), [{ label: 'Cancel' }, { label: 'Apply', primary: true, fn: () => go(o) }]); };
  const pct = a => a.map(v => [v, Math.round(v * 100) + '%']);
  const quantDialog = () => dlg('Quantize', [['grid', 'Grid', SNAPS().slice(1), V.snap || STEP], ['strength', 'Strength', pct([1, 0.75, 0.5, 0.25]), 1], ['ends', 'Lengths too', [[0, 'No'], [1, 'Yes']], 0]],
    o => { const s = V.snap; V.snap = o.grid; run('quantize', o.strength, !!o.ends); V.snap = s; });
  const strumDialog = () => dlg('Strum', [['step', 'Time between notes', [[2, 'Very tight'], [4, 'Tight'], [8, 'Medium'], [16, 'Loose'], [24, 'Very loose']], 4]], o => run('strum', o.step), 'Chords roll in from the lowest note up, like a guitar.');
  const limitDialog = () => dlg('Limit range', [['lo', 'Lowest note', Array.from({ length: 97 }, (_, i) => [i + 12, noteName(i + 12)]), 48], ['hi', 'Highest note', Array.from({ length: 97 }, (_, i) => [i + 12, noteName(i + 12)]), 72]],
    o => run('limit', Math.min(o.lo, o.hi), Math.max(o.lo, o.hi)), 'Notes outside are moved by octaves until they fit.');
  const randDialog = () => dlg('Randomize', [['vel', 'Velocity', [[0, 'Leave'], [0.2, 'A little'], [0.4, 'Some'], [0.8, 'A lot']], 0.2], ['time', 'Timing', [[0, 'Leave'], [4, 'A little'], [8, 'Some'], [16, 'A lot']], 4], ['pan', 'Pan', [[0, 'Leave'], [0.3, 'A little'], [0.7, 'A lot']], 0]], o => run('randomize', o));
  const levelsDialog = () => dlg('Scale levels', [['lo', 'Velocity at the start', pct([0.1, 0.25, 0.5, 0.75, 1]), 0.25], ['hi', 'Velocity at the end', pct([0.1, 0.25, 0.5, 0.75, 1]), 1]], o => run('levels', o.lo, o.hi), 'A ramp of velocities across the notes, for fades and builds.');
  const lfoDialog = () => dlg('LFO', [['lane', 'Changes', [['v', 'Velocity'], ['pan', 'Note pan'], ['fine', 'Note fine pitch'], ['rel', 'Note release'], ['cut', 'Note filter cutoff'], ['res', 'Note filter resonance']], 'v'], ['period', 'One cycle every', [[0.25, '1 beat'], [0.5, '2 beats'], [1, '1 bar'], [2, '2 bars'], [4, '4 bars']], 1], ['depth', 'Depth', pct([0.1, 0.25, 0.5, 0.75, 1]), 0.5], ['center', 'Centre (velocity)', pct([0.25, 0.5, 0.75]), 0.5]], o => run('lfo', o));
  const PROGS = { 'I – V – vi – IV (pop)': [0, 4, 5, 3], 'vi – IV – I – V': [5, 3, 0, 4], 'I – vi – IV – V (50s)': [0, 5, 3, 4], 'ii – V – I – I (jazz)': [1, 4, 0, 0], 'i – VI – III – VII (minor pop)': [0, 5, 2, 6], 'i – iv – v – i': [0, 3, 4, 0], 'I – IV – V – IV': [0, 3, 4, 3] };
  const progDialog = () => dlg('Chord progression', [['prog', 'Progression', Object.keys(PROGS).map(k => [k, k]), Object.keys(PROGS)[0]], ['len', 'Each chord lasts', [[BEAT * 2, '2 beats'], [BAR, '1 bar'], [BAR * 2, '2 bars']], BAR], ['oct', 'Octave', [[2, '2'], [3, '3'], [4, '4'], [5, '5']], 4]],
    o => { run('progression', { degrees: PROGS[o.prog], len: o.len, oct: o.oct, at: 0 }); }, 'Built from the key chosen above (major if none). The new chords are selected, ready to move or arpeggiate.');
  // Slide notes start no voice of their own: the notes already sounding glide to their pitch over their length.
  const slideMode = () => { V.slide = !V.slide; toolbar(); toast(V.slide ? 'New notes are slide notes' : 'New notes are normal notes'); };
  const slideSel = () => { const ts = targets(); if (!ts.length) return; const on = !ts.every(n => n.slide); for (const n of ts) if (on) n.slide = true; else delete n.slide; done(); };
  const toolsMenu = e => menuAt(e.currentTarget, [
    { head: sel.size ? `${sel.size} selected` : 'All notes' },
    { label: 'Generate chord progression…', fn: progDialog }, '-',
    { label: 'Quick legato', hint: 'Ctrl+L', fn: () => run('legato') }, { label: 'Quick quantize', hint: 'Ctrl+Q', fn: () => run('quantize') }, { label: 'Quick quantize starts + lengths', fn: () => run('quantize', 1, true) }, { label: 'Quantize…', hint: 'Alt+Q', fn: quantDialog }, '-',
    { label: 'Quick chop', hint: 'Ctrl+U', fn: () => run('chop') }, { label: 'Glue', hint: 'Ctrl+G', fn: () => run('glue') }, '-',
    { label: 'Arpeggiate…', fn: arpDialog }, { label: 'Strum…', hint: 'Alt+S', fn: strumDialog }, { label: 'Flam', fn: () => run('flam') }, { label: 'Limit…', fn: limitDialog },
    { label: 'Flip (mirror pitches)', fn: () => run('flip') }, { label: 'Reverse in time', fn: () => run('reverse') }, { label: 'Randomize…', fn: randDialog }, { label: 'Humanize', fn: () => run('humanize') }, '-',
    { label: 'Scale levels…', fn: levelsDialog }, { label: 'LFO…', fn: lfoDialog },
    { label: 'Make slide notes / back to normal', hint: 'Shift+S', fn: slideSel }, { label: 'Draw slide notes', hint: 'S', on: V.slide, fn: slideMode },
    '-',
    { label: 'Length × 2', fn: () => run('scaleLen', 2) }, { label: 'Length ÷ 2', fn: () => run('scaleLen', 0.5) },
    { label: 'Transpose +1 octave', fn: () => run('transpose', 12) }, { label: 'Transpose −1 octave', fn: () => run('transpose', -12) },
    '-',
    { label: 'Detect key from notes', fn: () => { const ns = notes(); if (ns.length < 3) return toast('Add a few notes first'); const k = keyFromNotes(ns); S.p.key = { root: k.root, scale: k.minor ? 'minor' : 'major' }; commit(); toast(`Looks like ${k.name}`); } },
    { label: 'Select all', hint: 'Ctrl+A', fn: () => { notes().forEach(n => sel.add(n.id)); req(); } },
    { label: 'Duplicate', hint: 'Ctrl+B', fn: () => run('dup') }, { label: 'Delete', hint: 'Del', fn: () => run('del') },
  ]);

  cv.addEventListener('keydown', e => {
    const id = e.key === 'Backspace' ? 'roll.del' : actionFor(e, 'roll'), s = V.snap || STEP;
    const nudge = d => { if (!sel.size) return; for (const n of targets()) n.t = Math.max(0, n.t + d); done(); };
    const tr = d => { if (sel.size) run('transpose', d); };
    const acts = {
      'roll.del': () => run('del'), 'roll.all': () => { notes().forEach(n => sel.add(n.id)); req(); }, 'roll.none': () => { sel.clear(); req(); }, 'roll.copy': () => T.copy(), 'roll.cut': () => { T.copy(); run('del'); },
      'roll.paste': () => run('paste'), 'roll.dup': () => run('dup'), 'roll.quant': () => run('quantize'), 'roll.quant2': () => run('quantize'), 'roll.quantDlg': quantDialog,
      'roll.legato': () => run('legato'), 'roll.chop': () => run('chop'), 'roll.glue': () => run('glue'), 'roll.strum': strumDialog, 'roll.slide': slideMode, 'roll.slideSel': slideSel,
      'roll.up': () => tr(1), 'roll.down': () => tr(-1), 'roll.octUp': () => tr(12), 'roll.octDown': () => tr(-12), 'roll.sUp': () => tr(1), 'roll.sDown': () => tr(-1),
      'roll.left': () => nudge(-s), 'roll.right': () => nudge(s), 'roll.left2': () => nudge(-s), 'roll.right2': () => nudge(s),
    };
    if (id && id.startsWith('roll.t.')) { V.tool = id.slice(7); toolbar(); e.preventDefault(); e.stopPropagation(); return; }
    if (!id || !acts[id]) return;
    acts[id](); e.preventDefault(); e.stopPropagation();
  });
  function toolbar() {
    const c = cur(), k = S.p.key || (S.p.key = { root: 0, scale: 'chromatic' });
    const tool = (id, title) => h('button', { class: 'tb' + (V.tool === id ? ' on' : ''), title, onclick: () => { V.tool = id; toolbar(); } }, TOOL_GLYPH[id] || icon(id));
    const p = pat(), lp = p.loop;
    const stampOpts = [['', 'Stamp: off'], ...Object.keys(CHORDS).map(k => [k, 'Chord · ' + k]), ...Object.keys(STAMP_SCALES).map(k => [k, 'Scale · ' + k])];
    const tog = (key, label, title) => h('button', { class: 'btn sm' + (prefs[key] ? ' on' : ''), title, onclick: () => { prefs[key] = !prefs[key]; savePrefs(); toolbar(); req(); } }, label);
    bar.replaceChildren(
      mkSel(S.p.channels.map(x => [x.id, x.name]), c.id, v => { S.ch = v; bus.emit('sel'); }, { class: 'chsel', title: 'Channel being edited' }),
      h('div', { class: 'seg' }, tool('draw', 'Draw (P): tap to add, drag to move, drag the right edge to resize'), tool('paint', 'Paint (B): drag along a row to paint notes'), tool('select', 'Select (E): drag a box'), tool('slice', 'Slice (C)'),
        tool('erase', 'Delete (D) - or right-click'), tool('mute', 'Mute (T): click or drag over notes to mute / unmute them'), tool('zoom', 'Zoom (Z): click to zoom in, right-click to zoom out'), tool('play', 'Play (Y): click to play the pattern from there'), tool('pan', 'Pan the view')),
      h('label', null, 'Snap'), mkSel(SNAPS(), V.snap, v => { V.snap = +v; req(); }),
      mkSel(stampOpts, V.stamp, v => { V.stamp = v; }, { title: 'Stamp: drawing one note adds a whole chord or scale' }),
      h('label', null, 'Key'), mkSel(NOTES.map((n, i) => [i, n]), k.root, v => { k.root = +v; commit(''); req(); }), mkSel(Object.keys(SCALES), k.scale, v => { k.scale = v; commit(''); req(); }, { class: 'scalesel', title: 'Scale' }),
      h('button', { class: 'btn sm' + (V.slide ? ' on' : ''), title: 'Slide (S): new notes are slide notes. A slide note glides the notes already sounding to its pitch over its length, and plays as a normal note when nothing is sounding. Shift+S turns the selected notes into slide notes or back.', onclick: slideMode }, 'Slide'),
      tog('scaleLock', 'Snap to scale', 'Snap new and moved notes to the key and scale chosen beside it'),
      h('button', { class: 'btn sm' + (lp && lp.on ? ' on' : ''), title: 'Loop mode: play only part of the pattern, over and over. Drag on the ruler to set the range.', onclick: () => {
        if (lp && lp.on) lp.on = false;
        else { const ts = notes().filter(n => sel.has(n.id)); p.loop = ts.length ? { on: true, a: Math.floor(Math.min(...ts.map(n => n.t)) / BEAT) * BEAT, b: Math.ceil(Math.max(...ts.map(n => n.t + n.d)) / BEAT) * BEAT } : lp && lp.b > lp.a ? { ...lp, on: true } : { on: true, a: 0, b: Math.min(BAR, p.bars * BAR) }; }
        commit(''); toolbar(); req();
      } }, 'Loop'),
      tog('ghost', 'Ghost', 'Show other channels faintly'), tog('follow', 'Follow', 'Scroll along with the playhead while playing (off: the view stays where you are working)'),
      h('button', { class: 'btn', onclick: toolsMenu }, 'Tools ▾'),
      // finger-friendly editing, shown on touch screens in place of the keyboard shortcuts
      h('div', { class: 'seg touchonly', title: 'Editing without a keyboard' }, btn('All', () => { const ns = notes(); if (ns.length && ns.every(n => sel.has(n.id))) sel.clear(); else ns.forEach(n => sel.add(n.id)); req(); }, '', 'Select all notes (again: none)'),
        btn('Copy', () => T.copy()), btn('Paste', () => run('paste')), btn('Dup', () => run('dup'), '', 'Duplicate to the right'), btn('Del', () => run('del'), '', 'Delete the selected notes')),
      h('span', { class: 'grow' }), btn('−', () => zoom(0.75), 'sm', 'Zoom out'), btn('+', () => zoom(1.33), 'sm', 'Zoom in'));
  }

  bus.on('pos', t => {
    if (!el.isConnected) return;
    const x = t < 0 || S.mode === 'song' ? -1 : xOf(t);
    if (!prefs.follow) { /* stay put so you can keep editing while it plays */ }
    else if (x > W && engine.playing && !drag) { V.sx += (W - KW) * 0.9; req(); }
    else if (t >= 0 && x < KW && engine.playing && !drag && S.mode !== 'song') { V.sx = Math.max(0, t * V.zx); req(); }
    head.style.display = x >= KW && x <= W ? 'block' : 'none'; head.style.transform = `translateX(${x}px)`;
  });
  bus.on('key', ({ note, on }) => { on ? keysDown.add(note) : keysDown.delete(note); if (el.isConnected) req(); });
  const prune = () => { if (sel.size) { const ids = new Set(notes().map(n => n.id)); for (const id of sel) if (!ids.has(id)) sel.delete(id); } req(); };
  bus.on('project', () => { toolbar(); prune(); });
  bus.on('notes', prune);
  let lastCh = null;
  bus.on('sel', () => { sel.clear(); toolbar(); if (lastCh !== S.ch) { lastCh = S.ch; focusNotes(); } req(); });
  bus.on('theme', theme);
  new ResizeObserver(resize).observe(wrap);
  theme(); toolbar();
  return { el, show() { lastCh = S.ch; requestAnimationFrame(() => { resize(); focusNotes(); }); } };
}
