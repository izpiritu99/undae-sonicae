// Synth presets: each takes a default patch and returns the modified patch.
const mod = (s, i, src, dst, amt) => { s.mod[i] = { src, dst, amt }; };
export const PRESETS = {
  'Init Saw': s => s,
  'Sub Bass': s => {
    s.osc[0].wave = 'sawtooth'; s.sub = 0.7; s.mode = 'mono'; s.glide = 0.04;
    Object.assign(s.filter, { cutoff: 420, q: 2, env: 26 }); s.fenv = { a: 0.003, d: 0.22, s: 0.15, r: 0.15 }; s.aenv = { a: 0.004, d: 0.3, s: 0.75, r: 0.12 };
    return s;
  },
  'Soft Lead': s => {
    s.osc[0].wave = 'square'; s.osc[1] = { on: true, wave: 'sawtooth', oct: 0, semi: 0, fine: 9, level: 0.45 };
    Object.assign(s.filter, { cutoff: 2600, q: 3, env: 10, key: 0.4 }); s.aenv = { a: 0.012, d: 0.25, s: 0.7, r: 0.3 };
    s.lfo[0].rate = 5.5; mod(s, 0, 'lfo1', 'pitch', 0.008); mod(s, 1, 'wheel', 'cutoff', 0.5);
    return s;
  },
  'Wide Pad': s => {
    s.uni = 5; s.detune = 22; s.spread = 0.85; s.osc[1] = { on: true, wave: 'sawtooth', oct: 1, semi: 0, fine: -6, level: 0.3 };
    Object.assign(s.filter, { cutoff: 1500, q: 1.5, env: 12 }); s.fenv = { a: 1.2, d: 2, s: 0.6, r: 1.5 }; s.aenv = { a: 0.6, d: 1, s: 0.85, r: 1.6 };
    s.lfo[1].rate = 0.18; mod(s, 0, 'lfo2', 'cutoff', 0.12); mod(s, 1, 'lfo2', 'pan', 0.3);
    return s;
  },
  Pluck: s => {
    Object.assign(s.filter, { cutoff: 500, q: 2.5, env: 44 }); s.fenv = { a: 0.002, d: 0.16, s: 0, r: 0.16 }; s.aenv = { a: 0.002, d: 0.32, s: 0, r: 0.25 };
    s.osc[1] = { on: true, wave: 'square', oct: 1, semi: 0, fine: 4, level: 0.25 }; mod(s, 0, 'vel', 'cutoff', 0.3);
    return s;
  },
  'FM Bell': s => {
    s.osc[0].wave = 'sine'; s.osc[1] = { on: false, wave: 'sine', oct: 1, semi: 7, fine: 0, level: 0.5 }; s.fm = 0.45; s.filter.on = false;
    s.aenv = { a: 0.002, d: 1.6, s: 0, r: 1.2 }; s.menv = { a: 0.002, d: 0.5, s: 0, r: 0.4 }; mod(s, 0, 'menv', 'fm', 0.5);
    return s;
  },
  'Acid Line': s => {
    s.mode = 'mono'; s.glide = 0.06; Object.assign(s.filter, { cutoff: 260, q: 13, env: 42, drive: 0.35 });
    s.fenv = { a: 0.002, d: 0.2, s: 0.05, r: 0.1 }; s.aenv = { a: 0.003, d: 0.2, s: 0.8, r: 0.08 };
    return s;
  },
  Organ: s => {
    s.osc = [{ on: true, wave: 'sine', oct: 0, semi: 0, fine: 0, level: 0.7 }, { on: true, wave: 'sine', oct: 1, semi: 0, fine: 0, level: 0.45 }, { on: true, wave: 'sine', oct: 1, semi: 7, fine: 0, level: 0.3 }];
    s.filter.on = false; s.aenv = { a: 0.006, d: 0.1, s: 1, r: 0.06 }; s.lfo[0].rate = 6.2; mod(s, 0, 'lfo1', 'amp', 0.15);
    return s;
  },
  'Noise Sweep': s => {
    s.osc[0].on = false; s.noise = 0.9; Object.assign(s.filter, { type: 'bandpass', cutoff: 900, q: 6 }); s.aenv = { a: 0.4, d: 1, s: 0.8, r: 1.2 };
    s.lfo[1].rate = 0.25; mod(s, 0, 'lfo2', 'cutoff', 0.5);
    return s;
  },
  'Reese Bass': s => {
    s.osc[1] = { on: true, wave: 'sawtooth', oct: 0, semi: 0, fine: 19, level: 0.75 }; s.uni = 3; s.detune = 18; s.spread = 0.6; s.mode = 'mono'; s.glide = 0.05; s.drift = 6;
    Object.assign(s.filter, { type: 'lowpass24', cutoff: 620, q: 2.2, drive: 0.25 }); s.aenv = { a: 0.01, d: 0.3, s: 0.9, r: 0.18 };
    s.lfo[1].rate = 0.14; mod(s, 0, 'lfo2', 'cutoff', 0.18); mod(s, 1, 'macro1', 'cutoff', 0.6);
    return s;
  },
  'Fold Lead': s => {
    s.osc[0].wave = 'sine'; s.osc[0].wave2 = 'triangle'; s.shaper = { type: 'fold', amt: 0.5 }; Object.assign(s.filter, { cutoff: 5200, q: 1.2 });
    s.aenv = { a: 0.008, d: 0.3, s: 0.75, r: 0.25 }; s.menv = { a: 0.005, d: 0.45, s: 0.2, r: 0.3 }; s.lfo[0].rate = 5.2; s.lfo[0].fade = 0.4;
    mod(s, 0, 'menv', 'oscA', 0.5); mod(s, 1, 'lfo1', 'pitch', 0.006); mod(s, 2, 'wheel', 'morph', 1);
    return s;
  },
  'Vocal Pad': s => {
    Object.assign(s.osc[0], { wave: 'vocal', wave2: 'organ', morph: 0.3 }); s.osc[1] = { on: true, wave: 'softsaw', oct: 1, semi: 0, fine: -7, level: 0.25, pan: 0.3 };
    s.uni = 3; s.detune = 16; s.spread = 0.8; Object.assign(s.filter, { cutoff: 2400, q: 1 }); Object.assign(s.filter2, { on: true, type: 'highpass', cutoff: 220, q: 0.7 });
    s.aenv = { a: 0.7, d: 1.2, s: 0.85, r: 1.8 }; s.lfo[1].rate = 0.11; s.lfo[1].wave = 'sine'; mod(s, 0, 'lfo2', 'morph', 0.45); mod(s, 1, 'rand', 'pan', 0.4);
    return s;
  },
  'Glass Keys': s => {
    s.osc[0].wave = 'bell'; s.osc[1] = { on: true, wave: 'sine', oct: 1, semi: 0, fine: 3, level: 0.3 }; s.filter.on = false;
    s.aenv = { a: 0.002, d: 1.4, s: 0, r: 0.9 }; s.vel = 0.8; mod(s, 0, 'vel', 'oscB', 0.4);
    return s;
  },
  'S&H Bleeps': s => {
    s.osc[0].wave = 'pulse25'; Object.assign(s.filter, { type: 'bandpass', cutoff: 1200, q: 3.5 }); s.gain = 1.4; s.aenv = { a: 0.003, d: 0.2, s: 0.6, r: 0.15 };
    s.lfo[2] = { wave: 'sh', rate: 8, sync: 0.25, fade: 0 }; mod(s, 0, 'lfo3', 'cutoff', 0.55); mod(s, 1, 'lfo3', 'pan', 0.5);
    return s;
  },
  'Ring Metal': s => {
    s.osc[0].wave = 'sine'; s.osc[0].level = 0.3; s.osc[1] = { on: false, wave: 'sine', oct: 1, semi: 6, fine: 0, level: 0.5 }; s.ring = 0.8;
    Object.assign(s.filter, { cutoff: 7000 }); s.aenv = { a: 0.002, d: 0.9, s: 0.1, r: 0.7 }; s.menv = { a: 0.002, d: 0.6, s: 0, r: 0.4 }; mod(s, 0, 'menv', 'ring', 0.4);
    return s;
  },};
