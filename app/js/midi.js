// Web MIDI input (USB / Bluetooth keyboards) and Standard MIDI File import/export.
import { bus, PPQ } from './util.js';
import { engine } from './audio/engine.js';
import { prefs } from './state.js';
import { onControl } from './links.js';

const curve = v => (prefs.velCurve === 'soft' ? v ** 0.55 : prefs.velCurve === 'hard' ? v ** 1.8 : prefs.velCurve === 'fixed' ? 0.8 : v);

function onMsg(e) {
  const [st, a, b] = e.data, cmd = st & 0xf0;
  if (st >= 0xf0 || (prefs.midiCh && (st & 15) !== prefs.midiCh - 1) || prefs.midiOff[e.target.name || e.target.id]) return;
  midi.last = `ch ${(st & 15) + 1} · ${cmd === 0x90 ? 'note on' : cmd === 0x80 ? 'note off' : cmd === 0xb0 ? 'CC' : cmd === 0xe0 ? 'bend' : 'msg'} ${a}`;
  if (cmd === 0xb0 && onControl(st & 15, a, b)) return;   // a knob linked to something in the app (or being linked now)
  if (cmd === 0x90 && b > 0) engine.noteOn(a, curve(b / 127));
  else if (cmd === 0x80 || cmd === 0x90) engine.noteOff(a);
  else if (cmd === 0xb0 && a === 1) engine.setWheel(b / 127);
  else if (cmd === 0xb0 && a === 64) { if (prefs.pedal) engine.setSustain(b >= 64); }
  else if (cmd === 0xe0) engine.setBend(((b << 7 | a) - 8192) / 8192);
  if (cmd === 0x90 || cmd === 0x80) bus.emit('midiact');
}
export const midi = {
  ok: false, inputs: [], err: '', last: '',
  async init() {
    if (!navigator.requestMIDIAccess) { this.err = 'Web MIDI is not available in this browser/webview.'; bus.emit('midi'); return false; }
    try {
      const acc = await navigator.requestMIDIAccess();
      const hook = () => { this.inputs = [...acc.inputs.values()].filter(i => i.state !== 'disconnected'); this.inputs.forEach(i => { i.onmidimessage = onMsg; }); bus.emit('midi'); };
      acc.onstatechange = hook; hook(); this.ok = true; this.err = '';
    } catch (e) { this.err = e.message || 'MIDI access was denied.'; }
    bus.emit('midi');
    return this.ok;
  },
  // Connect silently at startup only if permission was already granted, so there is no surprise prompt.
  async auto() {
    try { const p = await navigator.permissions.query({ name: 'midi' }); if (p.state === 'granted') this.init(); } catch { /* permission API missing */ }
  },
};

const vlq = n => { const b = [n & 127]; while ((n >>= 7)) b.unshift((n & 127) | 128); return b; };
const u32 = n => [n >>> 24 & 255, n >>> 16 & 255, n >>> 8 & 255, n & 255];
// tracks: [{ name, notes: [{ t, d, n, v }] }] in PPQ ticks. Returns a format-1 file.
export function writeMidi(tracks, bpm) {
  const out = [], push = a => { for (const x of a) out.push(x); };
  const chunk = body => { push([0x4d, 0x54, 0x72, 0x6b]); push(u32(body.length)); push(body); };
  push([0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 1, 0, tracks.length + 1, PPQ >> 8, PPQ & 255]);
  const us = Math.round(60e6 / bpm);
  chunk([0, 0xff, 0x51, 3, us >> 16 & 255, us >> 8 & 255, us & 255, 0, 0xff, 0x2f, 0]);
  tracks.forEach((tr, i) => {
    const ch = i % 16, ev = [], body = [], name = [...tr.name].map(c => c.charCodeAt(0) & 127);
    for (const n of tr.notes) { ev.push([Math.round(n.t), 1, 0x90 | ch, n.n, Math.max(1, Math.round(n.v * 127))]); ev.push([Math.round(n.t + n.d), 0, 0x80 | ch, n.n, 0]); }
    ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    body.push(0, 0xff, 3, ...vlq(name.length), ...name);
    let last = 0;
    for (const e of ev) { body.push(...vlq(e[0] - last), e[2], e[3], e[4]); last = e[0]; }
    body.push(0, 0xff, 0x2f, 0);
    chunk(body);
  });
  return new Uint8Array(out);
}
export function readMidi(buf) {
  const d = new DataView(buf); let p = 0;
  const str = () => { const s = String.fromCharCode(d.getUint8(p), d.getUint8(p + 1), d.getUint8(p + 2), d.getUint8(p + 3)); p += 4; return s; };
  const u = () => { const v = d.getUint32(p); p += 4; return v; };
  if (str() !== 'MThd') throw new Error('Not a MIDI file');
  const hl = u(), ntr = d.getUint16(p + 2), div = d.getUint16(p + 4); p += hl;
  if (div & 0x8000) throw new Error('SMPTE-timed MIDI files are not supported');
  const k = PPQ / div, notes = []; let bpm = 0;
  for (let t = 0; t < ntr && p < d.byteLength - 8; t++) {
    const id = str(), len = u(), end = p + len;
    if (id !== 'MTrk') { p = end; continue; }
    let time = 0, run = 0; const open = new Map();
    const vl = () => { let v = 0, b; do { b = d.getUint8(p++); v = v << 7 | (b & 127); } while (b & 128); return v; };
    while (p < end) {
      time += vl();
      let st = d.getUint8(p);
      if (st & 0x80) p++; else st = run;
      if (st === 0xff) { const type = d.getUint8(p++), l = vl(); if (type === 0x51 && !bpm) bpm = 60e6 / (d.getUint8(p) << 16 | d.getUint8(p + 1) << 8 | d.getUint8(p + 2)); p += l; continue; }
      if (st === 0xf0 || st === 0xf7) { p += vl(); continue; }
      run = st;
      const cmd = st & 0xf0, a = d.getUint8(p++), b = cmd === 0xc0 || cmd === 0xd0 ? 0 : d.getUint8(p++);
      if (cmd === 0x90 && b > 0) open.set(a, [time, b]);
      else if (cmd === 0x80 || cmd === 0x90) { const o = open.get(a); if (o) { notes.push({ t: Math.round(o[0] * k), d: Math.max(1, Math.round((time - o[0]) * k)), n: a, v: o[1] / 127, tr: t }); open.delete(a); } }
    }
    p = end;
  }
  return { notes, bpm: bpm ? Math.round(bpm * 10) / 10 : 0 };
}
