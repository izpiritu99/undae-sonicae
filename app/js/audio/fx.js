// Built-in effect processors for mixer FX slots. Each returns { inp, out, set(k, v), dispose? }.
import { bus } from '../util.js';
import { createBridge, hostUrl, bridgeAvailable, isWindowsSlot, plugServer } from './bridge.js';
const P = (k, label, min, max, def, o = {}) => ({ k, label, min, max, def, ...o });
const C = (k, label, opts, def) => ({ k, label, opts, def });
const hz = v => (v >= 1000 ? (v / 1000).toFixed(1) + 'k' : Math.round(v) + '');
const db = v => v.toFixed(1) + 'dB';
const pct = v => Math.round(v * 100) + '%';
const ms = v => Math.round(v) + 'ms';

function wetDry(ctx, sv) {
  const inp = ctx.createGain(), out = ctx.createGain(), dry = ctx.createGain(), wet = ctx.createGain();
  inp.connect(dry); dry.connect(out); wet.connect(out);
  return { inp, out, wet, mix(v) { sv(dry.gain, Math.cos(v * Math.PI / 2)); sv(wet.gain, Math.sin(v * Math.PI / 2)); } };
}
function impulse(ctx, sec, damp) {
  const sr = ctx.sampleRate, n = Math.max(64, Math.floor(sr * sec)), b = ctx.createBuffer(2, n, sr), a = 1 - damp * 0.92;
  for (let c = 0; c < 2; c++) {
    const d = b.getChannelData(c); let lp = 0;
    for (let i = 0; i < n; i++) { lp += a * ((Math.random() * 2 - 1) - lp); d[i] = lp * (1 - i / n) ** 2.6; }
  }
  return b;
}
const tanhCurve = (() => { const c = new Float32Array(2048); for (let i = 0; i < 2048; i++) c[i] = Math.tanh((i / 1023.5 - 1) * 3); return c; })();
function lfo(ctx, rate) { const o = ctx.createOscillator(); o.frequency.value = rate; o.start(); return o; }

const FX_DEFS = {
  eq: {
    name: 'EQ', params: [P('low', 'Low', -18, 18, 0, { fmt: db, bi: 1 }), P('mid', 'Mid', -18, 18, 0, { fmt: db, bi: 1 }), P('midf', 'Mid Hz', 200, 8000, 1000, { curve: 'exp', fmt: hz }), P('high', 'High', -18, 18, 0, { fmt: db, bi: 1 })],
    make(ctx, sv) {
      const a = ctx.createBiquadFilter(), b = ctx.createBiquadFilter(), c = ctx.createBiquadFilter();
      a.type = 'lowshelf'; a.frequency.value = 120; b.type = 'peaking'; b.Q.value = 0.9; c.type = 'highshelf'; c.frequency.value = 6000;
      a.connect(b); b.connect(c);
      return { inp: a, out: c, set(k, v) { sv(k === 'low' ? a.gain : k === 'mid' ? b.gain : k === 'midf' ? b.frequency : c.gain, v); } };
    },
  },
  filter: {
    name: 'Filter', params: [C('type', 'Type', ['lowpass', 'highpass', 'bandpass', 'notch'], 'lowpass'), P('cut', 'Cutoff', 30, 18000, 2000, { curve: 'exp', fmt: hz }), P('q', 'Reso', 0.1, 20, 1, { curve: 'exp' })],
    make(ctx, sv) {
      const f = ctx.createBiquadFilter();
      return { inp: f, out: f, set(k, v) { if (k === 'type') f.type = v; else sv(k === 'cut' ? f.frequency : f.Q, v); } };
    },
  },
  comp: {
    name: 'Compressor', params: [P('thr', 'Thresh', -60, 0, -18, { fmt: db }), P('ratio', 'Ratio', 1, 20, 4), P('att', 'Attack', 0.001, 0.3, 0.01, { curve: 'exp', fmt: v => ms(v * 1000) }), P('rel', 'Release', 0.02, 1, 0.15, { curve: 'exp', fmt: v => ms(v * 1000) }), P('knee', 'Knee', 0, 40, 12), P('gain', 'Makeup', 0, 24, 0, { fmt: db })],
    make(ctx, sv) {
      const c = ctx.createDynamicsCompressor(), g = ctx.createGain(); c.connect(g);
      const m = { thr: c.threshold, ratio: c.ratio, att: c.attack, rel: c.release, knee: c.knee };
      return { inp: c, out: g, set(k, v) { if (k === 'gain') sv(g.gain, 10 ** (v / 20)); else sv(m[k], v); } };
    },
  },
  limiter: {
    name: 'Limiter', params: [P('in', 'Input', -12, 24, 0, { fmt: db }), P('ceil', 'Ceiling', -24, 0, -1, { fmt: db }), P('rel', 'Release', 0.01, 0.6, 0.08, { curve: 'exp', fmt: v => ms(v * 1000) })],
    make(ctx, sv) {
      const g = ctx.createGain(), c = ctx.createDynamicsCompressor();
      c.knee.value = 0; c.ratio.value = 20; c.attack.value = 0.002; g.connect(c);
      return { inp: g, out: c, set(k, v) { if (k === 'in') sv(g.gain, 10 ** (v / 20)); else sv(k === 'ceil' ? c.threshold : c.release, v); } };
    },
  },
  reverb: {
    name: 'Reverb', params: [P('size', 'Size', 0.2, 8, 2.2, { curve: 'exp', fmt: v => v.toFixed(1) + 's' }), P('damp', 'Damp', 0, 1, 0.5, { fmt: pct }), P('pre', 'Pre-delay', 0, 200, 10, { fmt: ms }), P('mix', 'Mix', 0, 1, 0.3, { fmt: pct })],
    make(ctx, sv, live) {
      const m = wetDry(ctx, sv), pre = ctx.createDelay(0.5), cv = ctx.createConvolver();
      m.inp.connect(pre); pre.connect(cv); cv.connect(m.wet);
      let size = 2.2, damp = 0.5, tm = 0, first = true;
      const gen = () => { cv.buffer = impulse(ctx, size, damp); };
      return {
        inp: m.inp, out: m.out,
        set(k, v) {
          if (k === 'mix') return m.mix(v);
          if (k === 'pre') return sv(pre.delayTime, v / 1000);
          if (k === 'size') size = v; else damp = v;
          clearTimeout(tm);
          if (live && !first) tm = setTimeout(gen, 60); else { gen(); if (k === 'damp') first = false; }
        },
      };
    },
  },
  delay: {
    name: 'Delay', params: [P('time', 'Time', 20, 1500, 375, { curve: 'exp', fmt: ms }), P('fb', 'Feedback', 0, 0.95, 0.4, { fmt: pct }), P('tone', 'Tone', 300, 16000, 5000, { curve: 'exp', fmt: hz }), P('mix', 'Mix', 0, 1, 0.3, { fmt: pct })],
    make(ctx, sv) {
      const m = wetDry(ctx, sv), d = ctx.createDelay(2), fb = ctx.createGain(), lp = ctx.createBiquadFilter();
      m.inp.connect(d); d.connect(lp); lp.connect(fb); fb.connect(d); lp.connect(m.wet);
      return { inp: m.inp, out: m.out, set(k, v) { k === 'mix' ? m.mix(v) : sv(k === 'time' ? d.delayTime : k === 'fb' ? fb.gain : lp.frequency, k === 'time' ? v / 1000 : v); } };
    },
  },
  chorus: {
    name: 'Chorus', params: [P('rate', 'Rate', 0.05, 8, 0.8, { curve: 'exp', fmt: v => v.toFixed(2) + 'Hz' }), P('depth', 'Depth', 0, 1, 0.5, { fmt: pct }), P('mix', 'Mix', 0, 1, 0.5, { fmt: pct })],
    make(ctx, sv) {
      const m = wetDry(ctx, sv), o = lfo(ctx, 0.8), mg = ctx.createChannelMerger(2), ds = [];
      [0.013, 0.021].forEach((t, i) => {
        const d = ctx.createDelay(0.1), g = ctx.createGain(); d.delayTime.value = t; g.gain.value = 0;
        o.connect(g); g.connect(d.delayTime); m.inp.connect(d); d.connect(mg, 0, i); ds.push(g);
      });
      mg.connect(m.wet);
      return {
        inp: m.inp, out: m.out, dispose() { o.stop(); },
        set(k, v) { if (k === 'mix') m.mix(v); else if (k === 'rate') sv(o.frequency, v); else { sv(ds[0].gain, v * 0.006); sv(ds[1].gain, -v * 0.006); } },
      };
    },
  },
  phaser: {
    name: 'Phaser', params: [P('rate', 'Rate', 0.05, 8, 0.4, { curve: 'exp', fmt: v => v.toFixed(2) + 'Hz' }), P('depth', 'Depth', 0, 1, 0.7, { fmt: pct }), P('fb', 'Feedback', 0, 0.9, 0.4, { fmt: pct }), P('mix', 'Mix', 0, 1, 0.5, { fmt: pct })],
    make(ctx, sv) {
      const m = wetDry(ctx, sv), o = lfo(ctx, 0.4), dg = ctx.createGain(), fb = ctx.createGain(); o.connect(dg);
      let prev = m.inp, first;
      for (let i = 0; i < 4; i++) { const a = ctx.createBiquadFilter(); a.type = 'allpass'; a.frequency.value = 900; a.Q.value = 0.6; dg.connect(a.frequency); prev.connect(a); prev = a; first ||= a; }
      prev.connect(m.wet); prev.connect(fb); fb.connect(first);
      return { inp: m.inp, out: m.out, dispose() { o.stop(); }, set(k, v) { k === 'mix' ? m.mix(v) : sv(k === 'rate' ? o.frequency : k === 'depth' ? dg.gain : fb.gain, k === 'depth' ? v * 750 : v); } };
    },
  },
  dist: {
    name: 'Distortion', params: [P('drive', 'Drive', 0, 1, 0.4, { fmt: pct }), P('tone', 'Tone', 400, 18000, 8000, { curve: 'exp', fmt: hz }), P('level', 'Level', 0, 1.5, 0.7, { fmt: pct }), P('mix', 'Mix', 0, 1, 1, { fmt: pct })],
    make(ctx, sv) {
      const m = wetDry(ctx, sv), pre = ctx.createGain(), ws = ctx.createWaveShaper(), lp = ctx.createBiquadFilter(), lv = ctx.createGain();
      ws.curve = tanhCurve; ws.oversample = '2x'; m.inp.connect(pre); pre.connect(ws); ws.connect(lp); lp.connect(lv); lv.connect(m.wet);
      return { inp: m.inp, out: m.out, set(k, v) { k === 'mix' ? m.mix(v) : sv(k === 'drive' ? pre.gain : k === 'tone' ? lp.frequency : lv.gain, k === 'drive' ? 0.4 + v * v * 20 : v); } };
    },
  },
  crush: {
    name: 'Bitcrush', params: [P('bits', 'Bits', 2, 16, 8, { step: 1 }), P('tone', 'Tone', 400, 18000, 12000, { curve: 'exp', fmt: hz }), P('mix', 'Mix', 0, 1, 1, { fmt: pct })],
    make(ctx, sv) {
      const m = wetDry(ctx, sv), ws = ctx.createWaveShaper(), lp = ctx.createBiquadFilter();
      m.inp.connect(ws); ws.connect(lp); lp.connect(m.wet);
      return {
        inp: m.inp, out: m.out,
        set(k, v) {
          if (k === 'mix') return m.mix(v);
          if (k === 'tone') return sv(lp.frequency, v);
          const lv = 2 ** (v - 1), c = new Float32Array(4096);
          for (let i = 0; i < 4096; i++) c[i] = Math.round((i / 2047.5 - 1) * lv) / lv;
          ws.curve = c;
        },
      };
    },
  },
  duck: {
    name: 'Peak controller (duck)',
    params: [{ k: 'src', label: 'Triggered by', buses: true, def: '' }, C('mode', 'Duck', [['full', 'Whole signal'], ['band', 'One frequency band']], 'full'),
      P('depth', 'Depth', 0, 36, 12, { fmt: db }), P('sens', 'Sensitivity', 0.5, 12, 3), P('decay', 'Decay', 0.02, 1, 0.15, { curve: 'exp', fmt: v => ms(v * 1000) }),
      P('freq', 'Band Hz', 30, 12000, 120, { curve: 'exp', fmt: hz }), P('q', 'Band width', 0.3, 8, 1, { curve: 'exp' })],
    // An envelope follower on another bus (the "peak") pulls this bus down: either its whole level, or one EQ band of it.
    make(ctx, sv) {
      const G = v => { const g = ctx.createGain(); g.gain.value = v; return g; };
      const band = ctx.createBiquadFilter(), vca = G(1), side = G(1), abs = ctx.createWaveShaper(), sens = G(3), lp = ctx.createBiquadFilter(), lim = ctx.createWaveShaper(), toGain = G(0), toBand = G(0);
      band.type = 'peaking'; band.gain.value = 0; lp.type = 'lowpass'; lp.Q.value = 0.5; side.channelCount = 1; side.channelCountMode = 'explicit';
      abs.curve = Float32Array.from({ length: 1025 }, (_, i) => Math.abs(i / 512 - 1)); lim.curve = Float32Array.from({ length: 1025 }, (_, i) => Math.max(0, i / 512 - 1));
      band.connect(vca); toGain.connect(vca.gain); toBand.connect(band.gain);
      // Preferred: a real envelope follower (instant attack, adjustable release). Fallback: rectify + smooth.
      let env = null;
      if (ctx.__env) { env = new AudioWorkletNode(ctx, 'us-env', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [1], channelCount: 1, channelCountMode: 'explicit' }); side.connect(env); env.connect(toGain); env.connect(toBand); }
      else { side.connect(abs); abs.connect(sens); sens.connect(lp); lp.connect(lim); lim.connect(toGain); lim.connect(toBand); }
      let mode = 'full', depth = 12;
      const amt = () => { sv(toGain.gain, mode === 'full' ? -(1 - 10 ** (-depth / 20)) : 0); sv(toBand.gain, mode === 'band' ? -depth : 0); };
      return {
        inp: band, out: vca, side,
        set(k, v) {
          if (k === 'mode') { mode = v; amt(); } else if (k === 'depth') { depth = v; amt(); }
          else if (k === 'sens') { if (env) env.parameters.get('sens').value = v; else sv(sens.gain, v); }
          else if (k === 'decay') { if (env) env.parameters.get('release').value = v; else sv(lp.frequency, Math.max(6, 1 / (2 * Math.PI * v))); }
          else if (k === 'freq') sv(band.frequency, v); else if (k === 'q') sv(band.Q, v);
        },
      };
    },
  },
};

// Effects that only the native edition has so far (its built-in EQ, de-esser, saturator, reverbs, ... see HANDOVER.md):
// here they pass the audio through untouched and keep their settings, so a project that uses them still opens, plays
// (without that effect) and saves without losing anything.
const nativeOnly = type => ({ name: 'Native: ' + type, params: [], nativeOnly: true, make(ctx) { const g = ctx.createGain(); return { inp: g, out: g, set() {} }; } });
export const FX = new Proxy(FX_DEFS, { get: (t, k) => (typeof k !== 'string' || k in t) ? t[k] : nativeOnly(k) });

// Envelope follower used by the peak controller: output = how loud the input is right now (0..1).
const ENV = `registerProcessor('us-env', class extends AudioWorkletProcessor {
  static get parameterDescriptors() { return [{ name: 'sens', defaultValue: 3, automationRate: 'k-rate' }, { name: 'release', defaultValue: 0.15, automationRate: 'k-rate' }]; }
  constructor() { super(); this.e = 0; }
  process(i, o, p) {
    const x = i[0] && i[0][0], y = o[0][0]; if (!y) return true;
    const s = p.sens[0], a = Math.exp(-1 / (sampleRate * 0.002)), r = Math.exp(-1 / (sampleRate * Math.max(0.005, p.release[0]))); let e = this.e;
    for (let n = 0; n < y.length; n++) { const v = x ? Math.min(1, Math.abs(x[n]) * s) : 0; e = v + (e - v) * (v > e ? a : r); y[n] = e; }
    this.e = e; return true;
  }
});`;
// Must finish before a graph is built on this context (live or offline), or the peak controller uses its simpler fallback.
// A plug-in running in a host process: CLAP in native/host.c, Windows VST2 in native/win/vsthost.c under Wine (fmt 'vst2').
// The slot stores { path, pid, name, fmt }, the plug-in's saved state when it reports one, and one "v<id>" entry per
// plug-in parameter the user has changed. Audio passes straight through until the plug-in is loaded, and in offline export.
// Export: slot object -> AudioBuffer of that plug-in's output, rendered through its host before the offline graph is built.
export const offlineAudio = new Map();
// Bridge buffer sizes that this computer turned out to need, per kind of host, kept between sessions.
const learnedLatency = () => { try { return JSON.parse(localStorage.getItem('lw.pluglat')) || {}; } catch { return {}; } };
const remember = (kind, frames) => { const l = learnedLatency(); if (frames > (l[kind] || 0)) { l[kind] = frames; try { localStorage.setItem('lw.pluglat', JSON.stringify(l)); } catch { /* storage off */ } } };
FX.plugin = {
  name: 'Plug-in (CLAP / VST)', params: [],
  make(ctx, sv, live, p) {
    const inp = ctx.createGain(), out = ctx.createGain(); inp.connect(out);
    const x = { inp, out, info: null, bridge: null, key: '', status: live ? 'Choose a plug-in' : '', gen: 0 };
    if (!live && offlineAudio.has(p)) { const src = ctx.createBufferSource(); src.buffer = offlineAudio.get(p); inp.disconnect(out); src.connect(out); src.start(0); x.status = ''; }
    const unplug = () => { if (x.bridge) { try { inp.disconnect(x.bridge.node); } catch { /* not wired */ } x.bridge.close(); x.bridge = null; inp.connect(out); } x.info = null; x.guiOpen = false; };
    const say = s => { x.status = s; bus.emit('plugin'); };
    async function plug() {
      const gen = ++x.gen; x.key = p.path + '|' + p.pid; unplug();
      if (!p.path) return say('Choose a plug-in');
      if (!bridgeAvailable()) return say('This device cannot run plug-ins');
      const url = await hostUrl(p.fmt, p.bits, isWindowsSlot(p)), srv = plugServer();
      if (!url) return say(srv ? `The plug-in server ${srv.addr} is not answering (Settings › Plug-ins)` : !location.port && location.hostname === 'localhost' ? 'Plug-ins run on a computer: connect to one in Settings › Plug-ins' : isWindowsSlot(p) ? 'Windows plug-ins need Wine, and the Windows plug-in host is not running' : 'The plug-in host is not running');
      say('Loading…');
      let b;                                                             // Wine needs a little more buffer than the native host
      const kind = plugServer() ? 'remote' : isWindowsSlot(p) ? 'wine' : 'clap', learned = learnedLatency()[kind];   // over Wi-Fi: start at about 64 ms
      try { b = await createBridge(ctx, { url, latency: Math.max(learned || 0, { wine: 1536, clap: 1024, remote: 3072 }[kind]) }); } catch (e) { if (gen === x.gen) say(e.message); return; }
      const r = await b.ask({ op: 'load', path: p.path, id: p.pid || '', sampleRate: ctx.sampleRate });
      if (gen !== x.gen) return b.close();
      if (!r.ok) { b.close(); return say(r.error || 'The plug-in could not be loaded'); }
      if (p.state) await b.ask({ op: 'state', data: p.state });           // whatever the plug-in keeps beyond its parameters
      if (gen !== x.gen) return b.close();
      for (const q of r.params) if (typeof p['v' + q.id] === 'number') { q.value = p['v' + q.id]; b.send({ op: 'param', id: q.id, value: q.value }); }
      let saveT;
      b.onhost = m => {
        if (m.ev === 'params') { r.params = m.params; bus.emit('plugin'); }
        else if (m.ev === 'param') {                                      // changed in the plug-in's own window
          for (const ch of m.changes) { p['v' + ch.id] = ch.value; if (x.autoKey) bus.emit('autowrite', { target: x.autoKey + 'v' + ch.id, v: ch.value }); const q = r.params.find(z => z.id === ch.id); if (q) { q.value = ch.value; q.text = ch.text; } }
          bus.emit('plugin'); bus.emit('plugdirty');
        } else if (m.ev === 'gui') { x.guiOpen = m.open; bus.emit('plugin'); }
        else if (m.ev === 'dirty') { clearTimeout(saveT); saveT = setTimeout(async () => { const st = await b.ask({ op: 'state' }); if (st.ok && gen === x.gen) { p.state = st.data; bus.emit('plugdirty'); } }, 500); }
      };
      clearInterval(x.watch); x.watch = setInterval(() => { if (x.bridge === b) remember(kind, b.latency); }, 5000);   // a buffer that had to grow is where the next one starts
      x.bridge = b; x.info = r; try { inp.disconnect(out); } catch { /* not wired */ } inp.connect(b.node); b.node.connect(out);
      say(r.inputs || p.inst ? '' : 'This is an instrument: add it from the rack’s “+ Channel” menu to play it');
    }
    x.set = (k, v) => {
      if (!live) return;
      if (k === 'path' || k === 'pid') { if (p.path + '|' + p.pid !== x.key) plug(); }
      else if (k[0] === 'v' && x.bridge && typeof v === 'number') x.bridge.send({ op: 'param', id: +k.slice(1), value: v });
    };
    x.panic = () => { if (x.bridge) x.bridge.send({ op: 'panic' }); };   // cut everything, release tails included
    x.midi = (data, t) => { if (x.bridge) x.bridge.send(t == null ? { op: 'midi', data } : { op: 'midi', data, at: x.bridge.frameAt(t) }); };   // bend, mod wheel, sustain
    x.window = async show => { if (!x.bridge) return 'The plug-in is not loaded'; const r = await x.bridge.ask({ op: 'gui', show }); x.guiOpen = !!(r.ok && r.open); bus.emit('plugin'); return r.ok ? '' : r.error; };
    x.note = (on, key, vel = 0.8, t) => { if (x.bridge) x.bridge.send(t == null ? { op: 'note', on, key, vel } : { op: 'note', on, key, vel, at: x.bridge.frameAt(t) }); };   // t: context time, for notes scheduled ahead
    x.reload = () => { if (live && p.path) plug(); };   // the plug-in server changed: load again through the new one
    x.bind = np => { p = np; };                                           // the project object was replaced (undo, load): keep writing to the live one
    x.dispose = () => { x.gen++; clearInterval(x.watch); unplug(); };
    if (live && p.path) plug();
    return x;
  },
};

export async function loadFxWorklets(ctx) {
  try { await ctx.audioWorklet.addModule(URL.createObjectURL(new Blob([ENV], { type: 'application/javascript' }))); ctx.__env = true; } catch { /* worklets unavailable */ }
}

export const fxDefaults =type => Object.fromEntries(FX[type].params.map(q => [q.k, q.def]));

export function makeFx(ctx, type, p, live) {
  const d = FX[type];
  const sv = (param, v) => { if (live && ctx.state === 'running') param.setTargetAtTime(v, ctx.currentTime, 0.02); else param.value = v; };
  const node = d.make(ctx, sv, live, p), cache = {}, raw = node.set;
  node.set = (k, v) => { if (cache[k] === v) return; cache[k] = v; raw(k, v); };
  for (const q of d.params) node.set(q.k, p[q.k] ?? q.def);
  return node;
}
