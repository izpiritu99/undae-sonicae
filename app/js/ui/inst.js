// Instrument editor for the selected channel: modular synth or sampler.
import { h, sel, btn, knob, bus, toast, pickFile, drawWave, cssVar, noteName, ask, clamp } from '../util.js';
import { S, chan, commit, defSynth, fixSynth, busLabel } from '../state.js';
import { engine } from '../audio/engine.js';
import { pluginBody } from './plugui.js';
import { samples, addFile } from '../samples.js';
import { PRESETS } from '../presets.js';
import { MOD_SRC, MOD_DST, WAVES, LFO_WAVES, waveShape } from '../audio/synth.js';
import { toMono, decimate, yin } from '../audio/analysis.js';

const sec = v => (v < 1 ? Math.round(v * 1000) + 'ms' : v.toFixed(2) + 's');
const pct = v => Math.round(v * 100) + '%';
const hz = v => (v >= 1000 ? (v / 1000).toFixed(1) + 'k' : Math.round(v) + 'Hz');
const SRC_L = { none: '—', lfo1: 'LFO 1', lfo2: 'LFO 2', lfo3: 'LFO 3', menv: 'Mod env 1', menv2: 'Mod env 2', fenv: 'Filter env', vel: 'Velocity', key: 'Key track', wheel: 'Mod wheel', rand: 'Random per note', macro1: 'Macro 1', macro2: 'Macro 2', macro3: 'Macro 3', macro4: 'Macro 4' };
const DST_L = { none: '—', pitch: 'Pitch', cutoff: 'Filter 1 cutoff', res: 'Filter 1 reso', cutoff2: 'Filter 2 cutoff', amp: 'Amplitude', pan: 'Pan', fm: 'FM amount', ring: 'Ring mod', morph: 'Wave morph', oscA: 'Osc A level', oscB: 'Osc B level', oscC: 'Osc C level', sub: 'Sub level', noise: 'Noise level', lfo1rate: 'LFO 1 rate', lfo2rate: 'LFO 2 rate', lfo3rate: 'LFO 3 rate' };
const FTYPES = [['lowpass', 'Low-pass 12'], ['lowpass24', 'Low-pass 24'], ['highpass', 'High-pass 12'], ['highpass24', 'High-pass 24'], ['bandpass', 'Band-pass'], ['notch', 'Notch'], ['peaking', 'Peak'], ['allpass', 'Phaser (all-pass)']];
const SYNCS = [[0, 'Free (Hz)'], [16, '4 bars'], [8, '2 bars'], [4, '1 bar'], [2, '1/2'], [1, '1/4'], [0.5, '1/8'], [1 / 3, '1/8 triplet'], [0.25, '1/16']];
const FREQS = Float32Array.from({ length: 80 }, (_, i) => 20 * 1000 ** (i / 79));
let scratch = null;

export default function Inst() {
  const el = h('div', { class: 'view inst' });
  const done = () => commit('');
  const K = (obj, key, label, o = {}) => knob({ label, size: 'sm', value: obj[key], def: o.def ?? obj[key], onInput: v => { obj[key] = v; o.live && o.live(v); }, onChange: done, ...o });
  const card = (title, ...kids) => h('section', { class: 'card' }, h('h4', null, title), kids);
  const power = (obj, key = 'on') => { const b = h('button', { class: 'led' + (obj[key] ? ' on' : ''), title: 'On / off', onclick: () => { obj[key] = !obj[key]; b.classList.toggle('on', obj[key]); done(); } }); return b; };
  const pick = (label, opts, obj, key, after) => h('label', { class: 'pick' }, label, sel(opts, obj[key], v => { obj[key] = typeof obj[key] === 'number' ? +v : v; after && after(); done(); }));
  // Small line displays (wave shapes, filter curves, envelopes) share one drawing helper. pts are -1..1.
  const gfx = () => h('div', { class: 'envgfx' });
  const line = (box, d) => { box.innerHTML = `<svg viewBox="0 0 130 42" preserveAspectRatio="none"><path d="${d}" fill="none" stroke="currentColor" stroke-width="2" vector-effect="non-scaling-stroke"/></svg>`; };
  const plot = (box, pts) => line(box, 'M' + Array.from(pts, (v, i) => `${(i / (pts.length - 1) * 130).toFixed(1)} ${(21 - v * 18).toFixed(1)}`).join(' L'));

  function envCard(title, e) {
    const svg = gfx();
    const paint = () => {
      const a = Math.min(1, e.a / 2) * 30 + 2, d = Math.min(1, e.d / 2) * 30 + 2, r = Math.min(1, e.r / 3) * 30 + 2, y = 38 - e.s * 34;
      line(svg, `M0 40 L${a} 4 L${a + d} ${y} L${a + d + 26} ${y} L${a + d + 26 + r} 40`);
    };
    const t = { min: 0.001, max: 8, curve: 'exp', fmt: sec, live: paint };
    paint();
    return card(title, svg, h('div', { class: 'krow' }, K(e, 'a', 'Attack', t), K(e, 'd', 'Decay', t), K(e, 's', 'Sustain', { fmt: pct, live: paint }), K(e, 'r', 'Release', t)));
  }

  function synthUI(c) {
    const s = c.synth, now = () => engine.ctx.currentTime;
    const oscCard = (o, i) => {
      const box = gfx();
      const paint = () => { const a = waveShape(o.wave), b = waveShape(o.wave2), m = o.morph; plot(box, a.map((v, k) => v * (1 - m) + b[k] * m)); };
      paint();
      return card(h('span', null, power(o), ' Oscillator ' + 'ABC'[i]), box, h('div', { class: 'krow' },
        pick('Wave', WAVES, o, 'wave', paint), pick('Morph to', WAVES, o, 'wave2', paint), K(o, 'morph', 'Morph', { fmt: pct, def: 0, live: paint }),
        K(o, 'oct', 'Octave', { min: -3, max: 3, step: 1, bi: true, def: 0 }), K(o, 'semi', 'Semi', { min: -12, max: 12, step: 1, bi: true, def: 0 }),
        K(o, 'fine', 'Fine', { min: -100, max: 100, step: 1, bi: true, def: 0, fmt: v => v + 'c' }), K(o, 'level', 'Level', { fmt: pct }), K(o, 'pan', 'Pan', { min: -1, max: 1, bi: true, def: 0 })));
    };
    const filterCard = (X, which, title) => {
      const box = gfx(), steep = () => X.type.endsWith('24');
      const paint = () => {
        scratch ||= engine.ctx.createBiquadFilter();
        scratch.type = X.type.replace('24', ''); scratch.frequency.value = X.cutoff; scratch.Q.value = steep() ? X.q * 0.55 : X.q;
        const mag = new Float32Array(80), ph = new Float32Array(80); scratch.getFrequencyResponse(FREQS, mag, ph);
        plot(box, Array.from(mag, m => clamp(20 * Math.log10(Math.max(1e-4, steep() ? m * m : m)) / 30, -1, 1)));
      };
      const liveP = (prop, k = () => 1) => v => { paint(); engine.eachVoice(c.id, x => (x[which] || []).forEach(b => b[prop].setTargetAtTime(v * k(), now(), 0.02))); };
      paint();
      return card(h('span', null, power(X), ' ' + title), box, h('div', { class: 'krow' },
        pick('Type', FTYPES, X, 'type', paint),
        K(X, 'cutoff', 'Cutoff', { min: 30, max: 18000, curve: 'exp', fmt: hz, live: liveP('frequency') }), K(X, 'q', 'Reso', { min: 0.1, max: 24, curve: 'exp', live: liveP('Q', () => (steep() ? 0.55 : 1)) }),
        K(X, 'env', 'Env amt', { min: -72, max: 72, bi: true, def: 0, fmt: v => Math.round(v) + 'st' }), K(X, 'key', 'Key trk', { fmt: pct }),
        which === 'f1' ? K(X, 'drive', 'Drive', { fmt: pct }) : pick('Routing', [['serial', 'After filter 1'], ['parallel', 'Beside filter 1']], s, 'froute')));
    };
    const lfoCard = (l, i) => {
      const box = gfx();
      const paint = () => { const n = 96, w = l.wave, r = Array.from({ length: 9 }, (_, k) => Math.sin(k * 12.9898 + i) * 0.9); plot(box, Array.from({ length: n }, (_, k) => { const p = (k / n * 2) % 1, q = k / n * 8; return w === 'sine' ? Math.sin(2 * Math.PI * p) : w === 'triangle' ? 1 - 4 * Math.abs(((p + 0.25) % 1) - 0.5) : w === 'sawtooth' ? p * 2 - 1 : w === 'sawdown' ? 1 - p * 2 : w === 'square' ? (p < 0.5 ? 1 : -1) : w === 'sh' ? r[Math.floor(q)] : r[Math.floor(q)] + (r[Math.floor(q) + 1] - r[Math.floor(q)]) * (q % 1); })); };
      paint();
      return card('LFO ' + (i + 1), box, h('div', { class: 'krow' }, pick('Shape', LFO_WAVES, l, 'wave', paint), pick('Tempo sync', SYNCS, l, 'sync'),
        K(l, 'rate', 'Rate', { min: 0.02, max: 40, curve: 'exp', fmt: v => v.toFixed(2) + 'Hz' }), K(l, 'fade', 'Fade in', { min: 0, max: 4, fmt: sec, def: 0 })));
    };
    return [
      card('Preset', h('div', { class: 'hrow' }, sel(['— choose —', ...Object.keys(PRESETS)], '— choose —', v => { if (PRESETS[v]) { c.synth = fixSynth(PRESETS[v](defSynth())); commit(); toast('Loaded ' + v); } }),
        h('small', null, 'Changes apply to the next note; filter cutoff, resonance and macros respond while a note is held.'))),
      card('Macros', h('div', { class: 'krow' }, s.macro.map((_, i) => K(s.macro, i, 'Macro ' + (i + 1), { def: 0, fmt: pct, live: v => engine.eachVoice(c.id, x => x.macros && x.macros[i] && x.macros[i].setTargetAtTime(v, now(), 0.02)) }))),
        h('small', null, 'Four performance knobs. Pick “Macro 1-4” as a source in the matrix and one knob can sweep several things at once.')),
      ...s.osc.map(oscCard),
      card('Blend & unison', h('div', { class: 'krow' },
        K(s, 'uni', 'Voices', { min: 1, max: 9, step: 1 }), K(s, 'detune', 'Detune', { max: 60, fmt: v => Math.round(v) + 'c' }), K(s, 'spread', 'Stereo', { fmt: pct }), K(s, 'drift', 'Drift', { max: 30, def: 0, fmt: v => Math.round(v) + 'c' }),
        K(s, 'fm', 'FM B→A', { fmt: pct, def: 0 }), K(s, 'ring', 'Ring A×B', { fmt: pct, def: 0 }))),
      card('Sub & noise', h('div', { class: 'krow' },
        K(s, 'sub', 'Sub', { fmt: pct, def: 0 }), pick('Sub wave', WAVES.slice(0, 4), s, 'subWave'), pick('Sub octave', [[-1, '−1'], [-2, '−2']], s, 'subOct'),
        K(s, 'noise', 'Noise', { fmt: pct, def: 0 }), K(s, 'noiseTone', 'Noise tone', { fmt: v => (v > 0.97 ? 'white' : v > 0.6 ? 'pink-ish' : 'dark'), def: 1 }))),
      filterCard(s.filter, 'f1', 'Filter 1'), filterCard(s.filter2, 'f2', 'Filter 2'),
      card('Shaper (after filters)', h('div', { class: 'krow' },
        pick('Type', [['off', 'Off'], ['soft', 'Soft clip'], ['hard', 'Hard clip'], ['fold', 'Wavefolder'], ['asym', 'Asymmetric'], ['crush', 'Bit crush']], s.shaper, 'type'), K(s.shaper, 'amt', 'Amount', { fmt: pct }))),
      envCard('Amp envelope', s.aenv), envCard('Filter envelope', s.fenv), envCard('Mod envelope 1', s.menv), envCard('Mod envelope 2', s.menv2),
      ...s.lfo.map(lfoCard),
      card('Modulation matrix', h('div', { class: 'matrix' }, s.mod.map(m => h('div', { class: 'mrow' },
        sel(MOD_SRC.map(x => [x, SRC_L[x]]), m.src, v => { m.src = v; done(); }), h('span', null, '→'), sel(MOD_DST.map(x => [x, DST_L[x]]), m.dst, v => { m.dst = v; done(); }),
        K(m, 'amt', 'Amount', { min: -1, max: 1, bi: true, def: 0, size: 'xs', fmt: pct })))),
        h('small', null, 'Route any source to any destination. Stack several rows on one destination; amounts can be negative.')),
      card('Voice', h('div', { class: 'krow' },
        h('label', { class: 'pick' }, 'Mode', sel([['poly', 'Poly'], ['mono', 'Mono']], s.mode, v => { s.mode = v; done(); })),
        K(s, 'voices', 'Max voices', { min: 1, max: 32, step: 1 }), K(s, 'glide', 'Glide', { min: 0.001, max: 2, curve: 'exp', fmt: sec, def: 0.001 }),
        K(s, 'vel', 'Vel sens', { fmt: pct }), K(s, 'gain', 'Gain', { max: 1.5, fmt: pct }), K(s, 'bendRange', 'Bend', { min: 1, max: 24, step: 1, fmt: v => '±' + v }))),
    ];
  }
  function samplerUI(c) {
    const smp = samples.get(c.sample), cv = h('canvas', { class: 'wave' });
    requestAnimationFrame(() => {
      if (!smp || !cv.isConnected) return;
      drawWave(cv, smp.buffer.getChannelData(0), c.color);
      const g = cv.getContext('2d'); g.fillStyle = cssVar('--text'); g.fillRect(c.start * cv.clientWidth, 0, 1.5, cv.clientHeight);
    });
    const detect = () => {
      if (!smp) return;
      const { x, sr } = decimate(toMono(smp.buffer), smp.buffer.sampleRate, 11025), W = 1024, tMax = Math.ceil(sr / 50), d = new Float32Array(tMax + 2), hits = [];
      for (let off = 0; off + W + tMax + 1 < x.length && hits.length < 60; off += 256) { const tau = yin(x, off, W, 4, tMax, d); if (tau) hits.push(69 + 12 * Math.log2(sr / tau / 440)); }
      if (hits.length < 2) return toast('No clear pitch found in this sample');
      const m = hits.sort((a, b) => a - b)[hits.length >> 1]; c.root = Math.round(m); c.fine = Math.round((c.root - m) * 100); commit(); toast(`Root set to ${noteName(c.root)} (${c.fine >= 0 ? '+' : ''}${c.fine} cents)`);
    };
    const opts = [...samples.values()].map(x => [x.id, (x.builtin ? '' : '★ ') + x.name]);
    if (!smp) opts.unshift([c.sample, '(missing sample)']);
    const flag = (key, label, title) => { const b = h('button', { class: 'btn sm' + (c[key] ? ' on' : ''), title, onclick: () => { c[key] = !c[key]; b.classList.toggle('on', c[key]); commit(key === 'rev' ? 'project' : ''); } }, label); return b; };
    return [
      card('Sound source', cv, h('div', { class: 'hrow' },
        sel(opts, c.sample, v => { c.sample = v; commit(); }),
        btn('Upload audio…', async () => { const [f] = await pickFile('audio/*'); if (!f) return; try { c.sample = await addFile(f); commit(); } catch { toast('Could not decode that file'); } }, 'primary'),
        btn('Detect pitch → root', detect, '', 'Analyse the sample so it plays in tune across the keyboard')),
        h('small', null, 'Any audio you upload becomes this channel’s instrument: it is re-pitched across the piano roll from its root note.')),
      card('Pitch & shape', h('div', { class: 'krow' },
        K(c, 'root', 'Root', { min: 0, max: 127, step: 1, fmt: noteName, def: 60 }), K(c, 'tune', 'Tune', { min: -24, max: 24, step: 1, bi: true, def: 0, fmt: v => v + 'st' }),
        K(c, 'fine', 'Fine', { min: -100, max: 100, step: 1, bi: true, def: 0, fmt: v => v + 'c' }), K(c, 'start', 'Start', { max: 0.95, fmt: pct, def: 0 }),
        K(c, 'att', 'Attack', { min: 0.001, max: 2, curve: 'exp', fmt: sec, def: 0.001 }), K(c, 'rel', 'Release', { min: 0.005, max: 4, curve: 'exp', fmt: sec, def: 0.08 })),
        h('div', { class: 'hrow' }, flag('gate', 'Hold', 'Stop the sample when the note ends (for instruments). Off = one-shot (for drums)'), flag('rev', 'Reverse', 'Play backwards'), flag('cut', 'Cut self', 'Each new note cuts the previous one'),
          h('label', { class: 'pick' }, 'Choke group', sel([[0, 'None'], 1, 2, 3, 4], c.choke, v => { c.choke = +v; done(); })))),
    ];
  }

  function build() {
    const c = chan();
    if (!c) return el.replaceChildren(h('small', null, 'No channel selected.'));
    const sync = () => engine.graph.sync();
    el.replaceChildren(
      h('div', { class: 'vbar' }, h('i', { class: 'dot', style: `background:${c.color}` }),
        h('button', { class: 'btn', title: 'Rename', onclick: async () => { const v = await ask('Channel name', c.name); if (v) { c.name = v; commit(); } } }, c.name),
        h('span', { class: 'tag' }, c.type === 'synth' ? 'Synth' : c.type === 'plugin' ? 'Plug-in' : 'Sampler'),
        h('label', null, 'Bus'), sel(S.p.buses.map(b => [b.id, busLabel(S.p, b.id) + ' · ' + b.name]), c.bus, v => { c.bus = v; sync(); done(); }),
        knob({ label: 'Vol', size: 'xs', max: 1.25, value: c.vol, def: 0.8, link: `ch:${c.id}:vol`, onInput: v => { c.vol = v; sync(); }, onChange: () => commit() }),
        knob({ label: 'Pan', size: 'xs', min: -1, max: 1, bi: true, value: c.pan, def: 0, link: `ch:${c.id}:pan`, onInput: v => { c.pan = v; sync(); }, onChange: () => commit() }),
        h('span', { class: 'grow' }), btn('▶ Test', () => engine.preview(c, c.type === 'synth' ? 60 : c.root, 0.85, 0.6), 'sm')),
      h('div', { class: 'cards' }, c.type === 'synth' ? synthUI(c) : c.type === 'plugin' ? pluginUI(c) : samplerUI(c)));
  }
  // A plug-in instrument: the same slot body as a mixer plug-in effect, fed by this channel's host connection.
  function pluginUI(c) {
    const s = engine.graph && engine.graph.strips.get(c.id);
    return [h('div', { class: 'fxcard' }, pluginBody(c.plug, s && s.plug, { want: 'inst', autoKey: 'plug:' + c.id + ':', onPick: () => { c.name = (c.plug.name || 'Plug-in').slice(0, 18); engine.graph.sync(); commit(); }, onChange: () => commit('') }))];
  }
  let plugDraw = 0;
  bus.on('plugin', () => { const c = chan(); if (el.isConnected && c && c.type === 'plugin' && !plugDraw) plugDraw = requestAnimationFrame(() => { plugDraw = 0; build(); }); });
  bus.on('project', () => el.isConnected && build());
  bus.on('sel', () => el.isConnected && build());
  bus.on('samples', () => el.isConnected && build());
  return { el, show: build };
}
