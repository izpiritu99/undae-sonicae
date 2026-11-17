// Project model, preferences, undo history and autosave.
import { uid, bus, BAR, STEP, setSig } from './util.js';
import { fxDefaults } from './audio/fx.js';
import { PRESETS } from './presets.js';

export const COLORS = ['#ff7a59', '#ffb84d', '#e6d15a', '#7bd88f', '#45d0c4', '#5aa9ff', '#9d8cff', '#f277c6'];

const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* storage full or blocked */ } },
};
export const prefs = Object.assign({
  theme: 'slate', accent: '', radius: 10, density: 'cozy', scale: 1, noteColor: 'channel', gridContrast: 1, stepShape: 'lozenge', motion: true, glow: true,
  layout: 'stack', deckA: 'rack', deckB: 'roll', split: 0.45,
  latency: 'interactive', lookahead: 0.12, maxVoices: 16, meterFps: 30,
  recQuant: 0, typing: true, octave: 3, keys: false, scaleLock: false, ghost: true,
  float: false, floatX: 140, floatY: 110, deckC: 'song', deckD: 'mixer', deckF: 'inst',
  bitDepth: 24, exportRate: 48000, compFormat: 'mp3', mp3Kbps: 320, opusKbps: 192,
  startup: 'blank', follow: false, bgDim: 0.6,
  timeFmt: 'bst', rackView: 'steps', loopRec: true, recWhat: { midi: true, audio: true, auto: true }, keymap: {},
  songSnap: 1, sinkId: '', sampleRate: 0, recOffset: 0, autosave: 0.7, backups: true, velCurve: 'linear', midiCh: 0, pedal: true, midiOff: {}, truncated: true,
}, JSON.parse(store.get('lw.prefs') || '{}'));
export const savePrefs = () => store.set('lw.prefs', JSON.stringify(prefs));

// Live UI state that is not part of the saved project.
export const S = { p: null, ch: null, pat: null, mode: 'pat', rec: false, metro: false, songStart: 0 };

export function defSynth() {
  return {
    osc: [
      { on: true, wave: 'sawtooth', oct: 0, semi: 0, fine: 0, level: 0.8, wave2: 'square', morph: 0, pan: 0 },
      { on: false, wave: 'square', oct: 0, semi: 0, fine: 7, level: 0.5, wave2: 'sine', morph: 0, pan: 0 },
      { on: false, wave: 'sine', oct: -1, semi: 0, fine: 0, level: 0.5, wave2: 'triangle', morph: 0, pan: 0 },
    ],
    uni: 1, detune: 14, spread: 0.5, drift: 0, sub: 0, subWave: 'sine', subOct: -1, noise: 0, noiseTone: 1, fm: 0, ring: 0,
    filter: { on: true, type: 'lowpass', cutoff: 6000, q: 1, env: 0, key: 0, drive: 0 },
    filter2: { on: false, type: 'highpass', cutoff: 200, q: 1, env: 0, key: 0 }, froute: 'serial',
    shaper: { type: 'off', amt: 0.3 },
    aenv: { a: 0.005, d: 0.25, s: 0.7, r: 0.2 }, fenv: { a: 0.005, d: 0.3, s: 0.2, r: 0.3 }, menv: { a: 0.01, d: 0.4, s: 0, r: 0.3 }, menv2: { a: 0.3, d: 1, s: 0.5, r: 0.5 },
    lfo: [{ wave: 'sine', rate: 5, sync: 0, fade: 0 }, { wave: 'triangle', rate: 0.4, sync: 0, fade: 0 }, { wave: 'sh', rate: 8, sync: 0, fade: 0 }],
    mod: Array.from({ length: 8 }, () => ({ src: 'none', dst: 'none', amt: 0 })),
    macro: [0, 0, 0, 0],
    mode: 'poly', glide: 0, voices: 8, vel: 0.6, gain: 0.7, bendRange: 2,
  };
}
export function newChannel(type, o = {}) {
  const n = S.p ? S.p.channels.length : 0;
  return {
    id: uid(), name: type === 'synth' ? 'Synth' : type === 'plugin' ? 'Plug-in' : 'Sampler', type, color: COLORS[n % COLORS.length], vol: 0.8, pan: 0, mute: false, solo: false,
    bus: S.p && S.p.buses[1] ? S.p.buses[1].id : 'master', root: 60, sample: 'inst:keys', tune: 0, fine: 0, start: 0, att: 0, rel: 0.08, gate: false, rev: false, cut: false, choke: 0,
    synth: type === 'synth' ? defSynth() : null, ...o,
  };
}
export const newPattern = (name, bars = 1) => ({ id: uid(), name, bars, notes: {} });
const newBus = (name, kind = 'insert', o = {}) => ({ id: uid(), name, kind, vol: 1, pan: 0, mute: false, solo: false, mono: false, width: 1, inv: false, sends: [], toMaster: true, fx: [], input: '', arm: false, mon: false, stereo: false, ...o });
// Short label used by the rack's bus box and the mixer strips: M = master, 1..n = inserts, A/B = sends.
export const busLabel = (P, id) => { const b = P.buses.find(x => x.id === id); if (!b || b.kind === 'master') return 'M'; if (b.kind === 'send') return 'AB'[P.buses.filter(x => x.kind === 'send').indexOf(b)] || 'S'; return String(P.buses.filter(x => x.kind === 'insert').indexOf(b) + 1); };
export const newFx = (type, p = {}) => ({ id: uid(), type, on: true, p: { ...fxDefaults(type), ...p } });
export const addBus = (name, kind = 'insert') => { const b = newBus(name, kind); const i = kind === 'send' ? -1 : S.p.buses.findIndex(x => x.kind === 'send'); S.p.buses.splice(i < 0 ? S.p.buses.length : i, 0, b); return b; };
// Would audio leaving bus `from` (through sends) ever arrive at bus `to`? Used to refuse feedback loops.
export const reaches = (P, from, to, seen = new Set()) => from === to || (!seen.has(from) && (seen.add(from), (P.buses.find(b => b.id === from) || { sends: [] }).sends.some(s => reaches(P, s.to, to, seen))));
// Saved mixer strip presets (level, pan, width, flags and the whole FX chain).
export const busPresets = () => { try { return JSON.parse(store.get('lw.buspresets')) || {}; } catch { return {}; } };
export const saveBusPreset = (name, b) => { const all = busPresets(); if (b) all[name] = JSON.parse(JSON.stringify({ vol: b.vol, pan: b.pan, width: b.width, mono: b.mono, inv: b.inv, fx: b.fx })); else delete all[name]; store.set('lw.buspresets', JSON.stringify(all)); };
export const loadBusPreset = (name, b) => { const p = busPresets()[name]; if (!p) return; Object.assign(b, JSON.parse(JSON.stringify(p))); b.fx.forEach(f => { f.id = uid(); }); };

// A truly empty project: no channels, and only the master bus (with a safety limiter).
export function blankProject() {
  return { v: 1, name: 'Untitled', bpm: 120, swing: 0, swingDiv: 16, sig: { n: 4, d: 4 }, key: { root: 0, scale: 'chromatic' }, channels: [], patterns: [newPattern('Pattern 1')],
    song: { tracks: 6, clips: [], audio: [], auto: [] }, buses: [newBus('Master', 'master', { id: 'master', vol: 0.9, fx: [newFx('limiter')] })] };
}
// What the program was showing when it was last closed, kept aside when a session starts blank.
export const stashLast = p => { if (p && (p.channels.length || p.song.clips.length || p.song.audio.length)) store.set('lw.last', JSON.stringify(p)); };
// Start-up templates: projects the user saved to start new ones from (lw.templates, { name: project }). Samples are not
// copied: a template refers to library samples the same way a project does.
export const templates = () => { try { return JSON.parse(store.get('lw.templates')) || {}; } catch { return {}; } };
export function saveTemplate(name, p) { const all = templates(); all[name] = JSON.parse(JSON.stringify(p)); try { store.set('lw.templates', JSON.stringify(all)); return true; } catch { return false; } }
export function deleteTemplate(name) { const all = templates(); delete all[name]; store.set('lw.templates', JSON.stringify(all)); if (prefs.startup === 'tpl:' + name) { prefs.startup = 'blank'; savePrefs(); } }
export function fromTemplate(name) { const t = templates()[name]; if (!t) return null; const p = JSON.parse(JSON.stringify(t)); p.name = 'Untitled'; return p; }
export const lastSession = () => { try { const p = JSON.parse(store.get('lw.last')); return p && p.channels && p.buses ? p : null; } catch { return null; } };

// Starter kit used by the demo: drum, keys, bass and lead channels on their own buses.
function kitProject() {
  const buses = [
    newBus('Master', 'master', { id: 'master', vol: 0.9, fx: [newFx('limiter')] }),
    newBus('Drums'), newBus('Keys'), newBus('Bass'), newBus('Lead'),
    newBus('Send A', 'send', { fx: [newFx('reverb', { mix: 1 })] }), newBus('Send B', 'send', { fx: [newFx('delay', { mix: 1 })] }),
  ];
  const [, d, k, b, l] = buses.map(x => x.id);
  const prev = S.p; S.p = { channels: [], buses };
  const ch = [];
  const add = (type, o) => { const c = newChannel(type, o); c.color = COLORS[ch.length % COLORS.length]; ch.push(c); S.p.channels = ch; return c; };
  add('sample', { name: 'Kick', sample: '808:kick', bus: d });
  add('sample', { name: 'Snare', sample: '808:snare', bus: d });
  add('sample', { name: 'Clap', sample: '808:clap', bus: d });
  add('sample', { name: 'Hat', sample: '808:hat', bus: d, choke: 1, vol: 0.6 });
  add('sample', { name: 'Open Hat', sample: '808:ohat', bus: d, choke: 1, vol: 0.55 });
  add('sample', { name: 'Keys', sample: 'inst:keys', bus: k, gate: true, rel: 0.3 });
  add('synth', { name: 'Bass', bus: b, synth: PRESETS['Sub Bass'](defSynth()) });
  add('synth', { name: 'Lead', bus: l, synth: PRESETS['Soft Lead'](defSynth()), vol: 0.6 });
  S.p = prev;
  buses[2].sends = [{ to: buses[5].id, amt: 0.25 }]; buses[4].sends = [{ to: buses[5].id, amt: 0.2 }, { to: buses[6].id, amt: 0.25 }];
  return { v: 1, name: 'Untitled', bpm: 120, swing: 0, key: { root: 0, scale: 'chromatic' }, channels: ch, patterns: [newPattern('Pattern 1')], swingDiv: 16, sig: { n: 4, d: 4 }, song: { tracks: 6, clips: [], audio: [], auto: [] }, buses };
}

export function demoProject() {
  const P = kitProject();
  Object.assign(P, { name: 'First Light', bpm: 92, swing: 0.18, key: { root: 9, scale: 'minor' } });
  const [kick, snare, clap, hat, ohat, keys, bass, lead] = P.channels.map(c => c.id);
  const beat = P.patterns[0], chords = newPattern('Chords', 2), bassP = newPattern('Bass', 2), leadP = newPattern('Lead', 2);
  beat.name = 'Beat';
  const st = (p, id, steps, n = 60, v = 0.8, d = 1) => { (p.notes[id] ||= []).push(...steps.map(s => ({ id: uid(), t: s * STEP, d: d * STEP, n, v }))); };
  st(beat, kick, [0, 7, 10]); st(beat, snare, [4, 12]); st(beat, clap, [12], 60, 0.5);
  st(beat, hat, [0, 4, 8, 12], 60, 0.8); st(beat, hat, [2, 6, 10], 60, 0.5); st(beat, ohat, [14], 60, 0.7);
  [57, 60, 64, 67].forEach(n => st(chords, keys, [0], n, 0.7, 15)); [53, 57, 60, 64].forEach(n => st(chords, keys, [16], n, 0.7, 15));
  st(bassP, bass, [0], 45, 0.9, 3); st(bassP, bass, [3, 6], 45, 0.7, 2); st(bassP, bass, [10], 45, 0.8, 4);
  st(bassP, bass, [16], 41, 0.9, 3); st(bassP, bass, [19, 22], 41, 0.7, 2); st(bassP, bass, [26], 41, 0.8, 3); st(bassP, bass, [30], 43, 0.8, 2);
  st(leadP, lead, [0], 76, 0.8, 5); st(leadP, lead, [6], 74, 0.7, 2); st(leadP, lead, [8], 72, 0.8, 6);
  st(leadP, lead, [18], 69, 0.7, 4); st(leadP, lead, [24], 72, 0.8, 4); st(leadP, lead, [28], 76, 0.8, 4);
  P.patterns.push(chords, bassP, leadP);
  const clip = (pat, track, start, len) => ({ id: uid(), pat: pat.id, track, start, len });
  P.song.clips = [clip(beat, 0, 0, 8), clip(chords, 1, 0, 8), clip(bassP, 2, 2, 6), clip(leadP, 3, 4, 4)];
  return P;
}

// Channels picked in the Rack (Ctrl+click names); the current channel when none are.
export const selectedChannels = () => { const ids = new Set((S.chSel || []).filter(id => S.p.channels.some(c => c.id === id))); if (!ids.size && S.ch) ids.add(S.ch); return ids; };
export const pat = () => S.p.patterns.find(x => x.id === S.pat) || S.p.patterns[0];
export const chan = () => S.p.channels.find(x => x.id === S.ch) || S.p.channels[0];
export const notesOf = (p, id) => (p.notes[id] ||= []);
export const songBars = P => Math.ceil([...P.song.clips, ...(P.song.audio || [])].reduce((m, c) => Math.max(m, c.start + c.len), 0) - 1e-6);
export const fitBars = ticks => { let b = 1; while (b * BAR < ticks && b < 64) b *= 2; return b; };

let hist = [], hi = -1, saveT, lastBackup = 0;
// Autosave after the chosen delay (0 = off) and keep a rolling set of timed backups.
function save() {
  clearTimeout(saveT);
  if (!prefs.autosave) return;
  saveT = setTimeout(() => {
    store.set('lw.project', hist[hi]);
    if (prefs.backups && Date.now() - lastBackup > 6e5) {
      lastBackup = Date.now();
      const list = backups(); list.unshift({ at: lastBackup, name: S.p.name, data: hist[hi] }); store.set('lw.backups', JSON.stringify(list.slice(0, 5)));
    }
  }, prefs.autosave * 1000);
}
// Change the time signature. Pattern clips keep their bar numbers; recorded audio keeps its place in time.
export function changeSig(n, d) {
  const old = BAR; S.p.sig = { n, d }; setSig(n, d);
  for (const a of S.p.song.audio) { a.start *= old / BAR; a.len *= old / BAR; }
  commit();
}
export const backups = () => { try { return JSON.parse(store.get('lw.backups')) || []; } catch { return []; } };
export const saveNow = () => { clearTimeout(saveT); store.set('lw.project', JSON.stringify(S.p)); };
// Push an undo snapshot and autosave. ev: event to broadcast ('project' = structure changed, 'notes', or '' for silent).
export function commit(ev = 'project') {
  noteIds(S.p);
  const snap = JSON.stringify(S.p);
  if (hist[hi] !== snap) { hist.length = hi + 1; hist.push(snap); if (hist.length > 80) hist.shift(); hi = hist.length - 1; }
  save();
  if (ev) bus.emit(ev);
}
export const histPos = () => hi;
export function undo(d = -1) {
  const n = hi + d;
  if (n < 0 || n >= hist.length) return;
  hi = n; setProject(JSON.parse(hist[hi]), false);
  save();
}
// Bring older projects and presets up to the current shape.
export function fixSynth(s) {
  const d = defSynth();
  for (const k in d) if (s[k] == null) s[k] = d[k];
  s.osc.forEach((o, i) => { for (const k in d.osc[i]) if (o[k] == null) o[k] = d.osc[i][k]; });
  while (s.lfo.length < d.lfo.length) s.lfo.push(d.lfo[s.lfo.length]);
  s.lfo.forEach((l, i) => { for (const k in d.lfo[i]) if (l[k] == null) l[k] = d.lfo[i][k]; });
  while (s.mod.length < d.mod.length) s.mod.push({ src: 'none', dst: 'none', amt: 0 });
  return s;
}
// Every note needs its own id: selection in the Piano Roll goes by id (imported MIDI used to arrive without them).
function noteIds(p) { for (const pt of p.patterns) for (const k in pt.notes) for (const n of pt.notes[k]) if (!n.id) n.id = uid(); }
function migrate(p) {
  noteIds(p);
  p.song.audio ||= []; p.swingDiv ||= 16; p.sig ||= { n: 4, d: 4 };
  p.song.auto ||= [];
  const sendBus = p.buses.filter(b => b.kind === 'send');
  for (const b of p.buses) {
    b.input ??= ''; b.arm = false; b.mon ??= false; b.stereo ??= false; b.toMaster ??= true;
    if (!b.sends) { b.sends = []; [b.sendA, b.sendB].forEach((a, i) => { if (a > 0 && sendBus[i] && b.kind === 'insert') b.sends.push({ to: sendBus[i].id, amt: a }); }); delete b.sendA; delete b.sendB; }
    b.sends = b.sends.filter(s => p.buses.some(x => x.id === s.to));
  }
  for (const c of p.channels) if (c.synth) fixSynth(c.synth);
}
export function setProject(p, reset = true) {
  migrate(p);
  S.p = p; setSig(p.sig.n, p.sig.d);
  if (!p.patterns.some(x => x.id === S.pat)) S.pat = p.patterns[0].id;
  if (!p.channels.some(x => x.id === S.ch)) S.ch = p.channels[0] ? p.channels[0].id : null;
  if (reset) { hist = [JSON.stringify(p)]; hi = 0; store.set('lw.project', hist[0]); }
  bus.emit('project');
}
export function loadSaved() {
  try { const p = JSON.parse(store.get('lw.project')); return p && p.channels && p.patterns && p.buses ? p : null; } catch { return null; }
}
