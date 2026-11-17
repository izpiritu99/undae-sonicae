// Sound Design: build a one-shot sample from scratch (tone + noise) or on top of an existing sample, then save it.
import { h, sel, btn, knob, bus, toast, drawWave, cssVar, ask, pickFile } from '../util.js';
import { S, chan, commit, newChannel } from '../state.js';
import { samples, addBuffer, addFile } from '../samples.js';
import { engine } from '../audio/engine.js';
import { FX, makeFx, loadFxWorklets, fxDefaults } from '../audio/fx.js';
import { renderOffline, hostCall } from '../audio/bridge.js';
import { choosePlugin, slotFor } from './plugui.js';
import { uid, menuAt } from '../util.js';

const hz = v => (v >= 1000 ? (v / 1000).toFixed(1) + 'k' : Math.round(v) + 'Hz');
const sec = v => (v < 1 ? Math.round(v * 1000) + 'ms' : v.toFixed(2) + 's');
const pct = v => Math.round(v * 100) + '%';
const base = () => ({
  len: 0.5,
  tone: { on: true, wave: 'sine', f0: 180, f1: 50, sweep: 0.08, level: 0.9, dec: 0.3, fm: 0, ratio: 2 },
  noise: { on: false, type: 'highpass', cut: 4000, q: 0.7, level: 0.5, dec: 0.1 },
  smp: { on: false, id: '', start: 0, semi: 0, rev: false, level: 0.8 },
  drive: 0, filt: { on: false, cut: 8000, cut2: 8000, q: 1 }, att: 0.001, fade: 0.02, norm: true,
  fx: [], tail: 1,   // effect chain after the sound (built-in mixer effects and plug-ins), and how long to let it ring on
});
const mk = o => { const d = base(); for (const k in o) d[k] = typeof o[k] === 'object' ? { ...d[k], ...o[k] } : o[k]; return d; };
const PRESETS = {
  Kick: () => mk({ len: 0.5, tone: { f0: 180, f1: 45, sweep: 0.07, dec: 0.35 }, drive: 0.25 }),
  '808 Sub': () => mk({ len: 1.6, tone: { f0: 120, f1: 43, sweep: 0.05, dec: 1.3 }, drive: 0.4 }),
  Snare: () => mk({ len: 0.3, tone: { wave: 'triangle', f0: 260, f1: 170, sweep: 0.04, dec: 0.12, level: 0.6 }, noise: { on: true, type: 'highpass', cut: 1800, level: 0.8, dec: 0.16 } }),
  'Closed Hat': () => mk({ len: 0.12, tone: { on: false }, noise: { on: true, type: 'highpass', cut: 7500, level: 0.9, dec: 0.04 } }),
  'Open Hat': () => mk({ len: 0.6, tone: { on: false }, noise: { on: true, type: 'highpass', cut: 6500, level: 0.9, dec: 0.35 } }),
  Tom: () => mk({ len: 0.5, tone: { f0: 220, f1: 110, sweep: 0.15, dec: 0.3 } }),
  Zap: () => mk({ len: 0.35, tone: { wave: 'sawtooth', f0: 4000, f1: 80, sweep: 0.12, dec: 0.25 }, filt: { on: true, cut: 9000, cut2: 600, q: 4 } }),
  Riser: () => mk({ len: 3, tone: { wave: 'sawtooth', f0: 80, f1: 1600, sweep: 3, dec: 6, level: 0.5 }, noise: { on: true, type: 'bandpass', cut: 3000, q: 2, level: 0.6, dec: 6 }, filt: { on: true, cut: 300, cut2: 12000, q: 3 }, att: 2.2, fade: 0.05 }),
  'FM Blip': () => mk({ len: 0.25, tone: { f0: 660, f1: 660, sweep: 0.05, dec: 0.12, fm: 0.6, ratio: 3.5 } }),
  Clap: () => mk({ len: 0.35, tone: { on: false }, noise: { on: true, type: 'bandpass', cut: 1400, q: 1.6, level: 1, dec: 0.18 }, drive: 0.3 }),
};

async function render(D) {
  const sr = 44100, n = Math.max(64, Math.floor(D.len * sr)), ctx = new OfflineAudioContext(1, n, sr), G = v => { const g = ctx.createGain(); g.gain.value = v; return g; };
  const sum = G(1), env = (p, level, dec) => { p.setValueAtTime(level, 0); p.setTargetAtTime(0, 0.002, Math.max(0.005, dec) / 3); };
  if (D.tone.on) {
    const T = D.tone, o = ctx.createOscillator(), g = G(0); o.type = T.wave;
    o.frequency.setValueAtTime(Math.max(10, T.f0), 0); o.frequency.exponentialRampToValueAtTime(Math.max(10, T.f1), Math.max(0.003, T.sweep));
    if (T.fm > 0.005) { const m = ctx.createOscillator(), mg = G(T.fm * T.f0 * 4); m.frequency.value = T.f0 * T.ratio; m.connect(mg); mg.connect(o.frequency); mg.gain.setTargetAtTime(0, 0.002, T.dec / 3); m.start(); }
    env(g.gain, T.level, T.dec); o.connect(g); g.connect(sum); o.start();
  }
  if (D.noise.on) {
    const N = D.noise, b = ctx.createBuffer(1, n, sr), d = b.getChannelData(0); for (let i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
    const s = ctx.createBufferSource(), f = ctx.createBiquadFilter(), g = G(0); s.buffer = b; f.type = N.type; f.frequency.value = N.cut; f.Q.value = N.q;
    env(g.gain, N.level, N.dec); s.connect(f); f.connect(g); g.connect(sum); s.start();
  }
  const src = D.smp.on && samples.get(D.smp.id);
  if (src) {
    let buf = src.buffer;
    if (D.smp.rev) { const r = ctx.createBuffer(buf.numberOfChannels, buf.length, buf.sampleRate); for (let c = 0; c < buf.numberOfChannels; c++) r.getChannelData(c).set(Float32Array.from(buf.getChannelData(c)).reverse()); buf = r; }
    const s = ctx.createBufferSource(), g = G(D.smp.level); s.buffer = buf; s.playbackRate.value = 2 ** (D.smp.semi / 12); s.connect(g); g.connect(sum); s.start(0, D.smp.start * buf.duration);
  }
  let tail = sum;
  if (D.drive > 0.01) { const ws = ctx.createWaveShaper(), k = 1 + D.drive * 24; ws.curve = Float32Array.from({ length: 1024 }, (_, i) => Math.tanh((i / 511.5 - 1) * k) / Math.tanh(k)); tail.connect(ws); tail = ws; }
  if (D.filt.on) { const f = ctx.createBiquadFilter(); f.Q.value = D.filt.q; f.frequency.setValueAtTime(D.filt.cut, 0); f.frequency.exponentialRampToValueAtTime(Math.max(20, D.filt.cut2), D.len); tail.connect(f); tail = f; }
  const out = G(0); out.gain.setValueAtTime(0, 0); out.gain.linearRampToValueAtTime(1, Math.min(D.len * 0.9, Math.max(0.0005, D.att)));
  out.gain.setValueAtTime(1, Math.max(0, D.len - D.fade)); out.gain.linearRampToValueAtTime(0, D.len);
  tail.connect(out); out.connect(ctx.destination);
  let buf = await ctx.startRendering();
  if ((D.fx || []).some(f => f.on)) buf = await effects(buf, D);
  const d = buf.getChannelData(0);
  if (D.norm && buf.numberOfChannels > 1) { let m = 0; for (let c = 0; c < buf.numberOfChannels; c++) for (const v of buf.getChannelData(c)) m = Math.max(m, Math.abs(v)); if (m > 1e-4) for (let c = 0; c < buf.numberOfChannels; c++) { const x = buf.getChannelData(c); for (let i = 0; i < x.length; i++) x[i] *= 0.95 / m; } return buf; }
  if (D.norm) { let m = 0; for (let i = 0; i < d.length; i++) { const a = Math.abs(d[i]); if (a > m) m = a; } if (m > 1e-4) { const k = 0.95 / m; for (let i = 0; i < d.length; i++) d[i] *= k; } }
  return buf;
}

// The effect chain, in order, on a stereo copy with D.tail seconds of room to ring out. Built-in effects run in an offline
// graph; a plug-in runs through its host (bridge.js renderOffline), with the knob values stored in its slot.
async function effects(src, D) {
  const sr = src.sampleRate, n = src.length + Math.floor((D.tail || 0) * sr);
  let L = new Float32Array(n), R = new Float32Array(n); L.set(src.getChannelData(0)); R.set(src.getChannelData(src.numberOfChannels > 1 ? 1 : 0));
  for (const f of D.fx) {
    if (!f.on) continue;
    if (f.type === 'plugin') { if (f.p.path) [L, R] = await renderOffline(f.p, sr, n, [L, R]); continue; }
    const ctx = new OfflineAudioContext(2, n, sr); await loadFxWorklets(ctx);
    const b = ctx.createBuffer(2, n, sr); b.copyToChannel(L, 0); b.copyToChannel(R, 1);
    const s = ctx.createBufferSource(), x = makeFx(ctx, f.type, f.p, false); s.buffer = b; s.connect(x.inp); x.out.connect(ctx.destination); s.start();
    const out = await ctx.startRendering(); L = out.getChannelData(0); R = out.getChannelData(1);
  }
  const out = new AudioBuffer({ numberOfChannels: 2, length: n, sampleRate: sr }); out.copyToChannel(L, 0); out.copyToChannel(R, 1); return out;
}

export default function Design() {
  const startBox = h('span', null), cv = h('canvas', { class: 'wave big' }), cards = h('div', { class: 'cards' }), info = h('small', null, '');
  let D = PRESETS.Kick(), buf = null, timer = 0, name = 'Kick', auto = true;
  const el = h('div', { class: 'view design' },
    h('div', { class: 'vbar' },
      h('label', null, 'Start from'), startBox,
      btn('▶ Play', () => play(), 'primary'), h('label', { class: 'chk' }, h('input', { type: 'checkbox', checked: true, onchange: e => { auto = e.target.checked; } }), 'Play on change'),
      h('span', { class: 'grow' }), info,
      btn('Save to library', () => save(), '', 'Keep this sound as a sample'), btn('+ Rack', () => save('rack'), '', 'Save and add as a new channel'), btn('→ Selected', () => save('sel'), '', 'Save and use for the selected sampler channel')),
    cv, cards);
  const play = () => buf && engine.previewBuffer(buf, 0.9);
  // Begin from one of your own sounds: the tone and noise layers start off, the sample plays untouched, ready to reshape.
  function fromSample(id) { const s = samples.get(id); if (!s) return; D = mk({ len: Math.min(30, Math.max(0.05, s.buffer.duration)), tone: { on: false }, smp: { on: true, id, level: 1 }, fade: 0.005 }); name = s.name + ' edit'; build(); update(true); }
  async function fromFile() { const [f] = await pickFile('audio/*'); if (!f) return; try { fromSample(await addFile(f)); } catch { toast('Could not decode that file'); } }
  function startMenu() {
    const mine = [...samples.values()].filter(s => !s.builtin);
    startBox.replaceChildren(sel([['', '— choose —'], ...Object.keys(PRESETS).map(k => ['p:' + k, 'Preset · ' + k]), ['file', 'Your own audio file…'], ...mine.map(s => ['s:' + s.id, '★ ' + s.name])], '', v => {
      if (v === 'file') fromFile(); else if (v.startsWith('s:')) fromSample(v.slice(2)); else if (v.startsWith('p:')) { D = PRESETS[v.slice(2)](); name = v.slice(2); build(); update(true); }
    }));
  }
  async function update(sound) {
    try { buf = await render(D); } catch (e) { info.textContent = 'Could not render: ' + e.message; return; }
    if (el.isConnected) drawWave(cv, buf.getChannelData(0), cssVar('--accent'));
    info.textContent = `${buf.duration.toFixed(2)} s · ${buf.numberOfChannels > 1 ? 'stereo' : 'mono'} · 44.1 kHz`;
    if (sound && auto) play();
  }
  const changed = () => { clearTimeout(timer); timer = setTimeout(() => update(true), 160); };
  async function save(to) {
    if (!buf) return;
    const v = await ask('Name for this sound', name); if (!v) return; name = v;
    const id = await addBuffer(v, buf), s = samples.get(id);
    if (to === 'rack') { const c = newChannel('sample', { name: v.slice(0, 18), sample: id, gate: false }); S.p.channels.push(c); S.ch = c.id; commit(); toast(`“${v}” added to the rack`); }
    else if (to === 'sel') { const c = chan(); if (!c || c.type !== 'sample') return toast('Saved to the library - select a sampler channel to use it'); c.sample = id; commit(); toast(`“${c.name}” now plays ${v}`); }
    else toast(`Saved “${s.name}” to the library`);
  }
  function build() {
    const K = (o, k, label, opt = {}) => knob({ label, size: 'sm', value: o[k], def: o[k], onInput: v => { o[k] = v; changed(); }, ...opt });
    const power = o => { const b = h('button', { class: 'led' + (o.on ? ' on' : ''), title: 'On / off', onclick: () => { o.on = !o.on; b.classList.toggle('on', o.on); changed(); } }); return b; };
    const pick = (label, opts, o, k) => h('label', { class: 'pick' }, label, sel(opts, o[k], v => { o[k] = v; changed(); }));
    const card = (title, ...kids) => h('section', { class: 'card' }, h('h4', null, title), kids);
    const T = D.tone, N = D.noise, M = D.smp, F = D.filt, own = [...samples.values()].map(s => [s.id, (s.builtin ? '' : '★ ') + s.name]);
    if (!M.id && own.length) M.id = own[0][0];
    startMenu();
    cards.replaceChildren(
      card(h('span', null, power(T), ' Tone'), h('div', { class: 'krow' },
        pick('Wave', [['sine', 'Sine'], ['triangle', 'Triangle'], ['sawtooth', 'Saw'], ['square', 'Square']], T, 'wave'),
        K(T, 'f0', 'Start pitch', { min: 20, max: 8000, curve: 'exp', fmt: hz }), K(T, 'f1', 'End pitch', { min: 20, max: 8000, curve: 'exp', fmt: hz }), K(T, 'sweep', 'Sweep time', { min: 0.003, max: 4, curve: 'exp', fmt: sec }),
        K(T, 'dec', 'Decay', { min: 0.01, max: 6, curve: 'exp', fmt: sec }), K(T, 'level', 'Level', { fmt: pct }), K(T, 'fm', 'FM amount', { fmt: pct }), K(T, 'ratio', 'FM ratio', { min: 0.25, max: 8 })),
        h('small', null, 'A pitch that falls fast makes kicks and toms; one that rises slowly makes risers; FM adds metallic bite.')),
      card(h('span', null, power(N), ' Noise'), h('div', { class: 'krow' },
        pick('Colour', [['highpass', 'Bright (high-pass)'], ['bandpass', 'Band'], ['lowpass', 'Dark (low-pass)']], N, 'type'),
        K(N, 'cut', 'Frequency', { min: 60, max: 16000, curve: 'exp', fmt: hz }), K(N, 'q', 'Focus', { min: 0.3, max: 12, curve: 'exp' }), K(N, 'dec', 'Decay', { min: 0.005, max: 6, curve: 'exp', fmt: sec }), K(N, 'level', 'Level', { fmt: pct }))),
      card(h('span', null, power(M), ' Layer an existing sample'), h('div', { class: 'krow' },
        pick('Sample', own, M, 'id'),
        btn('Upload audio…', async () => { const [f] = await pickFile('audio/*'); if (!f) return; try { M.id = await addFile(f); M.on = true; build(); changed(); } catch { toast('Could not decode that file'); } }, 'sm', 'Use one of your own audio files as the layer'),
        K(M, 'start', 'Start', { max: 0.95, fmt: pct }), K(M, 'semi', 'Pitch', { min: -24, max: 24, step: 1, bi: true, fmt: v => v + 'st' }), K(M, 'level', 'Level', { max: 1.5, fmt: pct }),
        h('button', { class: 'btn sm' + (M.rev ? ' on' : ''), onclick: e => { M.rev = !M.rev; e.currentTarget.classList.toggle('on', M.rev); changed(); } }, 'Reverse')),
        h('small', null, 'Reshape any sound you already have: trim its start, re-pitch or reverse it, then add tone, noise, drive and filtering on top.')),
      card('Shape', h('div', { class: 'krow' },
        K(D, 'len', 'Length', { min: 0.03, max: 30, curve: 'exp', fmt: sec }), K(D, 'att', 'Fade in', { min: 0.0005, max: 4, curve: 'exp', fmt: sec }), K(D, 'fade', 'Fade out', { min: 0.002, max: 2, curve: 'exp', fmt: sec }), K(D, 'drive', 'Drive', { fmt: pct }),
        h('button', { class: 'btn sm' + (D.norm ? ' on' : ''), title: 'Bring the loudest point up to just under full scale', onclick: e => { D.norm = !D.norm; e.currentTarget.classList.toggle('on', D.norm); changed(); } }, 'Normalise'))),
      card(h('span', null, power(F), ' Filter sweep'), h('div', { class: 'krow' },
        K(F, 'cut', 'Start', { min: 40, max: 16000, curve: 'exp', fmt: hz }), K(F, 'cut2', 'End', { min: 40, max: 16000, curve: 'exp', fmt: hz }), K(F, 'q', 'Resonance', { min: 0.3, max: 18, curve: 'exp' }))),
      fxCard(card, K));
  }
  // Effects after the sound: any number, in order, built-in or plug-in. The result becomes stereo.
  function fxCard(card, K) {
    D.fx ||= [];
    const add = e => { const at = e.currentTarget; menuAt(at, [{ head: 'Built-in' }, ...Object.entries(FX).filter(([t]) => t !== 'duck' && t !== 'plugin').map(([t, d]) => ({ label: d.name, fn: () => { D.fx.push({ id: uid(), type: t, on: true, p: fxDefaults(t) }); D.tail = Math.max(D.tail, t === 'reverb' || t === 'delay' ? 2 : 0); build(); changed(); } })),
      { head: 'Plug-ins' }, { label: 'Plug-in effect (CLAP / VST)…', fn: () => choosePlugin(at, 'fx', async pl => {
        const f = { id: uid(), type: 'plugin', on: true, p: slotFor(pl), params: [] }; D.fx.push(f); build();
        const r = await hostCall({ op: 'load', path: pl.path, id: pl.id, sampleRate: 44100 }, pl.format, 60000, pl.bits, !!pl.windows);   // to learn its parameters
        if (r.ok) f.params = r.params.filter(q => !q.hidden && !q.readonly).slice(0, 32); else toast(r.error || 'The plug-in could not be loaded');
        build(); changed();
      }) }]); };
    const item = (f, i) => {
      const d = FX[f.type], move = dir => { const j = i + dir; if (j < 0 || j >= D.fx.length) return; D.fx.splice(i, 1); D.fx.splice(j, 0, f); build(); changed(); };
      const body = f.type === 'plugin'
        ? [h('small', null, f.p.name || 'Plug-in'), ...(f.params || []).map(q => knob({ label: (q.name || '').slice(0, 12), size: 'sm', min: q.min, max: q.max, def: q.def, value: f.p['v' + q.id] ?? q.value, onInput: v => { f.p['v' + q.id] = v; changed(); } }))]
        : d.params.filter(q => !q.buses).map(q => q.opts ? h('label', { class: 'pick' }, q.label, sel(q.opts, f.p[q.k], v => { f.p[q.k] = v; changed(); })) : K(f.p, q.k, q.label, { min: q.min, max: q.max, curve: q.curve, step: q.step, bi: q.bi, fmt: q.fmt }));
      return h('div', { class: 'fxcard' + (f.on ? '' : ' off') },
        h('div', { class: 'fxtitle' }, h('button', { class: 'led' + (f.on ? ' on' : ''), title: 'On / off', onclick: () => { f.on = !f.on; build(); changed(); } }), h('b', null, f.type === 'plugin' ? 'Plug-in' : d.name), h('span', { class: 'grow' }),
          btn('↑', () => move(-1), 'sm', 'Earlier'), btn('↓', () => move(1), 'sm', 'Later'), btn('✕', () => { D.fx.splice(i, 1); build(); changed(); }, 'sm', 'Remove')),
        h('div', { class: 'krow wrap' }, body));
    };
    return card(h('span', null, 'Effects'), h('div', { class: 'hrow' }, h('button', { class: 'btn primary sm', onclick: add }, '+ Add effect'), K(D, 'tail', 'Ring-out', { min: 0, max: 8, fmt: sec })),
      ...D.fx.map(item), !D.fx.length && h('small', null, 'Add EQ, compression, reverb, delay, chorus, distortion and more, or your own CLAP and Windows VST effects. Ring-out lengthens the sound so reverb and delay tails are kept.'));
  }
  build();
  bus.on('samples', () => el.isConnected && build());
  bus.on('theme', () => buf && el.isConnected && drawWave(cv, buf.getChannelData(0), cssVar('--accent')));
  return { el, show() { build(); requestAnimationFrame(() => update(false)); } };
}
