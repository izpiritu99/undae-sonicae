// Voice builders. A synth voice is a short-lived Web Audio node graph; a sample voice is a pitched buffer player.
import { mtof, clamp } from '../util.js';

const noiseCache = new WeakMap(), revCache = new WeakMap(), curves = new Map(), pwCache = new WeakMap(), randCache = new WeakMap(), coefCache = new Map();
function noiseBuf(ctx) {
  let b = noiseCache.get(ctx);
  if (!b) { b = ctx.createBuffer(1, ctx.sampleRate * 2, ctx.sampleRate); const d = b.getChannelData(0); for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1; noiseCache.set(ctx, b); }
  return b;
}
// Random LFO tables: stepped (sample & hold) holds each value for STEP_LEN samples, smooth interpolates between single samples.
const STEP_LEN = 64;
function randBuf(ctx, stepped) {
  let m = randCache.get(ctx); if (!m) randCache.set(ctx, (m = {}));
  const key = stepped ? 's' : 'r';
  if (!m[key]) {
    const n = 64, len = stepped ? STEP_LEN : 1, b = ctx.createBuffer(1, n * len, ctx.sampleRate), d = b.getChannelData(0);
    for (let i = 0; i < n; i++) { const v = Math.random() * 2 - 1; for (let j = 0; j < len; j++) d[i * len + j] = v; }
    m[key] = b;
  }
  return m[key];
}
function curve(type, amt) {
  const key = type + Math.round(amt * 24);
  if (!curves.has(key)) {
    const c = new Float32Array(1024), a = Math.round(amt * 24) / 24, k = 1 + a * 28;
    for (let i = 0; i < 1024; i++) {
      const x = i / 511.5 - 1;
      c[i] = type === 'hard' ? clamp(x * (1 + a * 12), -1, 1)
        : type === 'fold' ? Math.sin(x * (1 + a * 7) * Math.PI / 2)
        : type === 'asym' ? (Math.tanh((x + 0.35 * a) * k * 0.4) - Math.tanh(0.35 * a * k * 0.4)) / Math.tanh(k * 0.4)
        : type === 'crush' ? Math.round(x * (2 + (1 - a) * 22)) / (2 + (1 - a) * 22)
        : Math.tanh(x * k) / Math.tanh(k);
    }
    curves.set(key, c);
  }
  return curves.get(key);
}
function reversed(ctx, buf) {
  let r = revCache.get(buf);
  if (!r) { r = ctx.createBuffer(buf.numberOfChannels, buf.length, buf.sampleRate); for (let c = 0; c < buf.numberOfChannels; c++) r.getChannelData(c).set(Float32Array.from(buf.getChannelData(c)).reverse()); revCache.set(buf, r); }
  return r;
}
const hold = (p, t) => { if (p.cancelAndHoldAtTime) p.cancelAndHoldAtTime(t); else p.cancelScheduledValues(t); };

// --- wave shapes ---------------------------------------------------------------
export const BASIC = ['sine', 'triangle', 'sawtooth', 'square'];
export const WAVES = [['sine', 'Sine'], ['triangle', 'Triangle'], ['sawtooth', 'Saw'], ['square', 'Square'], ['pulse25', 'Pulse 25%'], ['pulse10', 'Pulse 10%'], ['softsaw', 'Soft saw'], ['organ', 'Organ'], ['bell', 'Bell'], ['vocal', 'Vocal'], ['metal', 'Metal'], ['stair', 'Stair']];
// Harmonic recipes for the non-basic shapes (cosine terms in re, sine terms in im).
export function wtCoefs(name) {
  if (coefCache.has(name)) return coefCache.get(name);
  const N = 64, re = new Float32Array(N + 1), im = new Float32Array(N + 1), PI = Math.PI;
  for (let n = 1; n <= N; n++) {
    if (name === 'pulse25' || name === 'pulse10') { const d = name === 'pulse25' ? 0.25 : 0.1; re[n] = 2 * Math.sin(2 * PI * n * d) / (PI * n); im[n] = 2 * (1 - Math.cos(2 * PI * n * d)) / (PI * n); }
    else if (name === 'softsaw') im[n] = 1 / n ** 1.7;
    else if (name === 'organ') im[n] = [0, 1, 0.8, 0.6, 0.3, 0, 0.4, 0, 0.3][n] || 0;
    else if (name === 'bell') im[n] = [0, 1, 0, 0.5, 0, 0, 0.35, 0, 0, 0.25, 0, 0, 0.2, 0, 0, 0, 0.12][n] || 0;
    else if (name === 'vocal') im[n] = Math.exp(-((n - 6) ** 2) / 6) + 0.6 * Math.exp(-((n - 13) ** 2) / 10) + 0.5 / n;
    else if (name === 'metal') im[n] = n % 2 ? (n % 4 === 1 ? 1 : -1) / Math.sqrt(n) : 0.15 / n;
    else if (name === 'stair') im[n] = n % 4 ? 1 / n : 0;
  }
  const c = { re, im }; coefCache.set(name, c);
  return c;
}
function setWave(ctx, os, name) {
  if (BASIC.includes(name)) { os.type = name; return; }
  let m = pwCache.get(ctx); if (!m) pwCache.set(ctx, (m = new Map()));
  if (!m.has(name)) { const c = wtCoefs(name); m.set(name, ctx.createPeriodicWave(c.re, c.im)); }
  os.setPeriodicWave(m.get(name));
}
// One cycle of a shape as n points in -1..1, for the oscillator displays.
export function waveShape(name, n = 96) {
  const out = new Float32Array(n);
  if (BASIC.includes(name)) {
    for (let i = 0; i < n; i++) { const p = i / n; out[i] = name === 'sine' ? Math.sin(2 * Math.PI * p) : name === 'square' ? (p < 0.5 ? 1 : -1) : name === 'sawtooth' ? ((p + 0.5) % 1) * 2 - 1 : 1 - 4 * Math.abs(((p + 0.25) % 1) - 0.5); }
    return out;
  }
  const { re, im } = wtCoefs(name); let pk = 0;
  for (let i = 0; i < n; i++) { const p = 2 * Math.PI * i / n; let v = 0; for (let k = 1; k < re.length; k++) if (re[k] || im[k]) v += re[k] * Math.cos(k * p) + im[k] * Math.sin(k * p); out[i] = v; pk = Math.max(pk, Math.abs(v)); }
  for (let i = 0; i < n; i++) out[i] /= pk || 1;
  return out;
}

// Schedule an ADSR on an AudioParam. dur == null means "held until release()". Returns the time the tail is inaudible.
export function adsr(p, e, t, dur, peak, base = 0) {
  const A = Math.max(e.a, 0.002), D = Math.max(e.d, 0.005), R = Math.max(e.r, 0.005);
  p.setValueAtTime(base, t);
  if (dur != null && dur <= A) {
    p.linearRampToValueAtTime(base + peak * dur / A, t + dur);
    p.setTargetAtTime(base, t + dur, R / 4);
    return t + dur + R * 1.5;
  }
  p.linearRampToValueAtTime(base + peak, t + A);
  p.setTargetAtTime(base + peak * e.s, t + A, D / 3);
  if (dur == null) return Infinity;
  const lv = e.s + (1 - e.s) * Math.exp(-(dur - A) / (D / 3));
  p.setValueAtTime(base + peak * lv, t + dur);
  p.setTargetAtTime(base, t + dur, R / 4);
  return t + dur + R * 1.5;
}
function adsrRelease(p, e, tr, base = 0) {
  const R = Math.max(e.r, 0.005);
  hold(p, tr); p.setTargetAtTime(base, tr, R / 4);
  return tr + R * 1.5;
}

export const MOD_SRC = ['none', 'lfo1', 'lfo2', 'lfo3', 'menv', 'menv2', 'fenv', 'vel', 'key', 'wheel', 'rand', 'macro1', 'macro2', 'macro3', 'macro4'];
export const MOD_DST = ['none', 'pitch', 'cutoff', 'res', 'cutoff2', 'amp', 'pan', 'fm', 'ring', 'morph', 'oscA', 'oscB', 'oscC', 'sub', 'noise', 'lfo1rate', 'lfo2rate', 'lfo3rate'];
export const LFO_WAVES = [['sine', 'Sine'], ['triangle', 'Triangle'], ['sawtooth', 'Ramp up'], ['sawdown', 'Ramp down'], ['square', 'Square'], ['sh', 'Sample & hold'], ['rand', 'Smooth random']];

export function synthVoice(ctx, P, note, vel, t, dur, dest, o = {}) {
  const f = mtof(note), nodes = [], srcs = {}, lfos = {};
  const start = (n, off) => { off != null ? n.start(t, off) : n.start(t); nodes.push(n); return n; };
  const G = (v = 1) => { const g = ctx.createGain(); g.gain.value = v; return g; };
  const uses = d => P.mod.some(m => m.dst === d && m.src !== 'none' && m.amt);
  const amp = G(), trem = G(), pan = ctx.createStereoPanner(), mix = G(), pitch = G();
  const bend = start(ctx.createConstantSource()); bend.offset.value = o.bend || 0; bend.connect(pitch);

  // --- signal path: mix -> drive -> filters (serial / parallel) -> shaper -> amp
  const F = P.filter, F2 = P.filter2 || { on: false }, SH = P.shaper || { type: 'off' };
  let tail = mix;
  if (F.drive > 0.01) { const ws = ctx.createWaveShaper(); ws.curve = curve('soft', F.drive); tail.connect(ws); tail = ws; }
  const mkF = X => {
    if (!X.on) return null;
    const steep = X.type.endsWith('24'), type = X.type.replace('24', ''), list = [];
    for (let i = 0; i < (steep ? 2 : 1); i++) {
      const b = ctx.createBiquadFilter(); b.type = type; b.frequency.value = X.cutoff; b.Q.value = (steep ? X.q * 0.55 : X.q) * 4 ** (o.res || 0); b.detune.value = (X.key || 0) * (note - 60) * 100 + (o.cut || 0) * 3600;
      if (i) list[i - 1].connect(b); list.push(b);
    }
    return list;
  };
  const f1 = mkF(F), f2 = mkF(F2);
  if (f1 && f2 && P.froute === 'parallel') { const sum = G(0.7); tail.connect(f1[0]); tail.connect(f2[0]); f1.at(-1).connect(sum); f2.at(-1).connect(sum); tail = sum; }
  else for (const x of [f1, f2]) if (x) { tail.connect(x[0]); tail = x.at(-1); }
  if (SH.type !== 'off' && SH.amt > 0.005) { const ws = ctx.createWaveShaper(); ws.curve = curve(SH.type, SH.amt); ws.oversample = '2x'; tail.connect(ws); tail = ws; }
  tail.connect(amp); amp.connect(trem); trem.connect(pan); pan.connect(dest);

  // --- oscillators
  const uni = clamp(P.uni | 0, 1, 9), glide = o.glideFrom != null && P.glide > 0.001 && o.glideFrom !== note ? mtof(o.glideFrom) / f : 0, drift = P.drift || 0;
  const mkOsc = (wave, ratio, det, out) => {
    const os = ctx.createOscillator(); setWave(ctx, os, wave); os.detune.value = det + (drift ? (Math.random() * 2 - 1) * drift : 0);
    const fr = f * ratio;
    if (glide) { os.frequency.setValueAtTime(fr * glide, t); os.frequency.exponentialRampToValueAtTime(fr, t + P.glide); } else os.frequency.value = fr;
    pitch.connect(os.detune); os.connect(out);
    return start(os);
  };
  const oscGain = [], carriers = [], morphs = [], ratioOf = O => 2 ** (O.oct + O.semi / 12);
  P.osc.forEach((O, i) => {
    const g = oscGain[i] = G(0);
    if (!O.on) return;
    g.gain.value = O.level / Math.sqrt(uni);
    if (O.pan) { const pn = ctx.createStereoPanner(); pn.pan.value = O.pan; g.connect(pn); pn.connect(mix); } else g.connect(mix);
    const bank = (wave, out) => {
      let l = out, r = out;
      if (uni > 1 && P.spread > 0) { l = ctx.createStereoPanner(); r = ctx.createStereoPanner(); l.pan.value = -P.spread; r.pan.value = P.spread; l.connect(out); r.connect(out); }
      for (let u = 0; u < uni; u++) { const os = mkOsc(wave, ratioOf(O), O.fine + (uni > 1 ? (u / (uni - 1) * 2 - 1) * P.detune : 0), u % 2 ? r : l); if (i === 0) carriers.push(os); }
    };
    const mo = O.morph || 0;
    if (mo > 0.001 || uses('morph')) { const a = G(1 - mo), b = G(mo); a.connect(g); b.connect(g); bank(O.wave, a); bank(O.wave2 || 'sine', b); morphs.push([a.gain, b.gain]); }
    else bank(O.wave, g);
  });
  let fmG = null, ringG = null, subG = null, noiseG = null;
  if (carriers.length && (P.fm > 0.001 || uses('fm'))) {
    const B = P.osc[1]; fmG = G(P.fm * f * 6);
    mkOsc(B.wave, ratioOf(B), B.fine, fmG); carriers.forEach(c => fmG.connect(c.frequency));
  }
  if (P.ring > 0.001 || uses('ring')) {
    const A = P.osc[0], B = P.osc[1], rm = G(0); ringG = G(P.ring || 0);
    mkOsc(A.wave, ratioOf(A), A.fine, rm); mkOsc(B.wave, ratioOf(B), B.fine, rm.gain); rm.connect(ringG); ringG.connect(mix);
  }
  if (P.sub > 0.001 || uses('sub')) { subG = G(P.sub); subG.connect(mix); mkOsc(P.subWave || 'sine', 2 ** (P.subOct || -1), 0, subG); }
  if (P.noise > 0.001 || uses('noise')) {
    const s = ctx.createBufferSource(); noiseG = G(P.noise); s.buffer = noiseBuf(ctx); s.loop = true;
    const tone = P.noiseTone ?? 1;
    if (tone < 0.98) { const lp = ctx.createBiquadFilter(); lp.frequency.value = 80 * 250 ** tone; lp.Q.value = 0.5; s.connect(lp); lp.connect(noiseG); } else s.connect(noiseG);
    noiseG.connect(mix); start(s, Math.random());
  }

  const peak = (1 - P.vel + P.vel * vel) * P.gain * 0.5;
  const rel = e => (o.rel ? { ...e, r: e.r * 4 ** o.rel } : e);   // per-note release: x0.25 .. x4
  const AE = rel(P.aenv), end = adsr(amp.gain, AE, t, dur, peak);
  const envs = [];   // [param, env] pairs to release
  const envSrc = e => { e = rel(e); const n = start(ctx.createConstantSource()); adsr(n.offset, e, t, dur, 1); envs.push([n.offset, e]); return n; };
  let fe = null;
  for (const [X, list] of [[F, f1], [F2, f2]]) if (list && Math.abs(X.env || 0) > 0.01) { fe ||= envSrc(P.fenv); const g = G(X.env * 100); fe.connect(g); list.forEach(b => g.connect(b.detune)); }

  // --- modulation matrix: any source to any destination, summed at the destination param
  const mkLfo = i => {
    const lf = P.lfo[i]; if (!lf) return null;
    const rate = lf.sync ? (o.bpm || 120) / 60 / lf.sync : lf.rate; let n, out, rateP, k = 1;
    if (lf.wave === 'sh' || lf.wave === 'rand') {
      const stepped = lf.wave === 'sh'; k = (stepped ? STEP_LEN : 1) / ctx.sampleRate;
      n = ctx.createBufferSource(); n.buffer = randBuf(ctx, stepped); n.loop = true; n.playbackRate.value = rate * k; rateP = n.playbackRate; start(n, Math.random() * n.buffer.duration);
    } else { n = ctx.createOscillator(); n.type = lf.wave === 'sawdown' ? 'sawtooth' : lf.wave; n.frequency.value = rate; rateP = n.frequency; start(n); }
    out = n;
    if (lf.wave === 'sawdown' || lf.fade > 0.005) {
      const g = G(lf.wave === 'sawdown' ? -1 : 1); n.connect(g); out = g;
      if (lf.fade > 0.005) { const v = g.gain.value; g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(v, t + lf.fade); }
    }
    return { out, rateP, k };
  };
  const lfo = i => (lfos[i] ||= mkLfo(i));
  const macros = {};
  const src = k => {
    if (srcs[k]) return srcs[k];
    let n;
    if (k.startsWith('lfo')) { const l = lfo(+k[3] - 1); n = l && l.out; }
    else if (k === 'menv') n = envSrc(P.menv);
    else if (k === 'menv2') n = envSrc(P.menv2);
    else if (k === 'fenv') n = fe || (fe = envSrc(P.fenv));
    else {
      n = start(ctx.createConstantSource());
      if (k.startsWith('macro')) { const i = +k[5] - 1; n.offset.value = (P.macro && P.macro[i]) || 0; macros[i] = n.offset; }
      else n.offset.value = k === 'vel' ? vel : k === 'key' ? (note - 60) / 48 : k === 'rand' ? Math.random() * 2 - 1 : o.wheel || 0;
    }
    return (srcs[k] = n);
  };
  const cut = (list, sc) => (list ? list.map(b => [b.detune, sc]) : []);
  const dsts = {
    pitch: [[pitch.gain, 0]], cutoff: cut(f1, 4800), cutoff2: cut(f2, 4800), res: f1 ? f1.map(b => [b.Q, 20]) : [], amp: [[trem.gain, 1]], pan: [[pan.pan, 1]],
    fm: fmG ? [[fmG.gain, f * 6]] : [], ring: ringG ? [[ringG.gain, 1]] : [], morph: morphs.flatMap(([a, b]) => [[a, -1], [b, 1]]),
    oscA: [[oscGain[0].gain, 1]], oscB: [[oscGain[1].gain, 1]], oscC: [[oscGain[2].gain, 1]], sub: subG ? [[subG.gain, 1]] : [], noise: noiseG ? [[noiseG.gain, 1]] : [],
  };
  for (const m of P.mod) {
    if (m.src === 'none' || m.dst === 'none' || !m.amt) continue;
    const s = src(m.src); if (!s) continue;
    let targets = dsts[m.dst];
    if (m.dst === 'pitch') { const g = G(m.amt * 2400); s.connect(g); g.connect(pitch); continue; }   // pitch sums into the shared detune bus
    if (m.dst.endsWith('rate')) { const l = lfo(+m.dst[3] - 1); targets = l ? [[l.rateP, 20 * l.k]] : []; }
    for (const [target, scale] of targets || []) { const g = G(m.amt * scale); s.connect(g); g.connect(target); }
  }

  const v = { note, t, end, filts: [...(f1 || []), ...(f2 || [])], f1, f2, bend: bend.offset, bendBase: 0, wheel: srcs.wheel ? srcs.wheel.offset : null, macros };
  const stopAt = e => { v.end = e; for (const n of nodes) { try { n.stop(e); } catch { /* already stopped */ } } };
  bend.onended = () => { try { pan.disconnect(); } catch { /* gone */ } };
  if (end !== Infinity) stopAt(end);
  v.release = tr => {
    if (v.released) return; v.released = true;
    const e = adsrRelease(amp.gain, AE, tr);
    for (const [p, en] of envs) adsrRelease(p, en, tr);
    stopAt(e);
  };
  v.kill = tk => { v.released = true; hold(amp.gain, tk); amp.gain.setTargetAtTime(0, tk, 0.006); stopAt(tk + 0.06); };
  v.slide = (to, ts, d) => glideTo(bend.offset, (o.bend || 0) + (to - note) * 100, ts, d);
  return v;
}
// Slide notes: glide a voice's pitch offset (cents) to a new value from ts over d seconds.
function glideTo(p, cents, ts, d) {
  if (p.cancelAndHoldAtTime) p.cancelAndHoldAtTime(ts); else p.setValueAtTime(p.value, ts);
  p.linearRampToValueAtTime(cents, ts + Math.max(0.01, d));
}

export function sampleVoice(ctx, buf, C, note, vel, t, dur, dest, o = {}) {
  if (C.rev) buf = reversed(ctx, buf);
  const s = ctx.createBufferSource(), g = ctx.createGain();
  s.buffer = buf;
  const rate = 2 ** ((note - C.root + (C.tune || 0)) / 12), base = C.fine || 0;
  s.playbackRate.value = rate; s.detune.value = base + (o.bend || 0);
  const off = clamp(C.start || 0, 0, 0.98) * buf.duration, A = Math.max(C.att || 0, 0.001), R = Math.max(C.rel || 0, 0.005) * 4 ** (o.rel || 0);
  g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(vel ** 1.5, t + A);
  // per-note cutoff / resonance: a low-pass that closes from 20 kHz (cut 0) to about 160 Hz (cut -1); resonance x0.25 .. x4
  let lp = null; if (o.cut < 0 || o.res) { lp = ctx.createBiquadFilter(); lp.frequency.value = 20000 * 2 ** (Math.min(0, o.cut || 0) * 7); lp.Q.value = 0.707 * 4 ** (o.res || 0); s.connect(lp); lp.connect(g); } else s.connect(g);
  g.connect(dest); s.start(t, off);
  s.onended = () => { try { g.disconnect(); if (lp) lp.disconnect(); } catch { /* gone */ } };
  const v = { note, t, end: t + (buf.duration - off) / rate, bend: s.detune, bendBase: base, sample: true };
  const fade = (tr, tau) => {
    if (tr >= v.end) return;
    hold(g.gain, tr); g.gain.setTargetAtTime(0, tr, tau); v.end = tr + tau * 6;
    try { s.stop(v.end); } catch { /* already stopped */ }
  };
  if (C.gate && dur != null) fade(t + dur, R / 4);
  v.release = tr => { if (C.gate) fade(tr, R / 4); };
  v.kill = tk => fade(Math.max(tk, t), 0.006);
  v.slide = (to, ts, d) => glideTo(s.detune, base + (o.bend || 0) + (to - note) * 100, ts, d);
  return v;
}
