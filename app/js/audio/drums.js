// 808-style drum kit and two stock instruments, synthesised offline at startup (no sample files shipped).
const SR = 44100;

function render(dur, fn) {
  const c = new OfflineAudioContext(1, Math.ceil(SR * dur), SR);
  fn(c, c.destination);
  return c.startRendering().then(b => {
    const d = b.getChannelData(0); let pk = 0;
    for (let i = 0; i < d.length; i++) pk = Math.max(pk, Math.abs(d[i]));
    if (pk > 0) { const k = 0.9 / pk; for (let i = 0; i < d.length; i++) d[i] *= k; }
    const f = Math.min(d.length, 400);
    for (let i = 0; i < f; i++) d[d.length - 1 - i] *= i / f;
    return b;
  });
}
const osc = (c, type, f) => { const o = c.createOscillator(); o.type = type; o.frequency.value = f; o.start(0); return o; };
const env = (c, peak, dec, att = 0.001) => {
  const g = c.createGain();
  g.gain.setValueAtTime(0.0001, 0); g.gain.linearRampToValueAtTime(peak, att); g.gain.exponentialRampToValueAtTime(0.0001, att + dec);
  return g;
};
const noise = (c, dur) => {
  const b = c.createBuffer(1, Math.ceil(SR * dur), SR), d = b.getChannelData(0);
  for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
  const s = c.createBufferSource(); s.buffer = b; s.start(0); return s;
};
const filt = (c, type, f, q = 0.7) => { const b = c.createBiquadFilter(); b.type = type; b.frequency.value = f; b.Q.value = q; return b; };
const chain = (...n) => { for (let i = 0; i < n.length - 1; i++) n[i].connect(n[i + 1]); };

const sweep = (f0, f1, t, dec, dur) => () => render(dur, (c, out) => {
  const o = osc(c, 'sine', f0);
  o.frequency.setValueAtTime(f0, 0); o.frequency.exponentialRampToValueAtTime(f1, t);
  chain(o, env(c, 1, dec), out);
  chain(noise(c, 0.02), filt(c, 'lowpass', 3000), env(c, 0.25, 0.008), out);
});
const METAL = [205.3, 304.4, 369.6, 522.7, 540, 800];
const metal = (dec, bp, hp, dur) => () => render(dur, (c, out) => {
  const sum = c.createGain(); sum.gain.value = 0.2;
  METAL.forEach(f => osc(c, 'square', f).connect(sum));
  chain(sum, filt(c, 'bandpass', bp, 0.8), filt(c, 'highpass', hp), env(c, 0.8, dec), out);
});

const KIT = [
  ['kick', 'Kick', sweep(165, 46, 0.075, 0.55, 0.7)],
  ['kicklong', 'Kick Long', sweep(150, 41, 0.09, 1.3, 1.5)],
  ['snare', 'Snare', () => render(0.4, (c, out) => {
    chain(osc(c, 'sine', 185), env(c, 0.55, 0.12), out); chain(osc(c, 'sine', 331), env(c, 0.3, 0.07), out);
    chain(noise(c, 0.4), filt(c, 'highpass', 1600), env(c, 0.7, 0.19), out);
  })],
  ['clap', 'Clap', () => render(0.5, (c, out) => {
    const g = c.createGain(); g.gain.setValueAtTime(0, 0);
    [0, 0.011, 0.022].forEach(t => { g.gain.setValueAtTime(1, t); g.gain.exponentialRampToValueAtTime(0.15, t + 0.009); });
    g.gain.setValueAtTime(0.9, 0.033); g.gain.exponentialRampToValueAtTime(0.0001, 0.36);
    chain(noise(c, 0.5), filt(c, 'bandpass', 1150, 1.3), g, out);
  })],
  ['hat', 'Closed Hat', metal(0.05, 10000, 7000, 0.15)],
  ['ohat', 'Open Hat', metal(0.42, 10000, 7000, 0.6)],
  ['cymbal', 'Cymbal', metal(1.5, 6500, 3800, 1.8)],
  ['cowbell', 'Cowbell', () => render(0.5, (c, out) => {
    const g = c.createGain(); g.gain.setValueAtTime(0.7, 0); g.gain.exponentialRampToValueAtTime(0.25, 0.03); g.gain.exponentialRampToValueAtTime(0.0001, 0.42);
    const bp = filt(c, 'bandpass', 2640, 1); osc(c, 'square', 540).connect(bp); osc(c, 'square', 800).connect(bp); chain(bp, g, out);
  })],
  ['rim', 'Rimshot', () => render(0.1, (c, out) => {
    const hp = filt(c, 'highpass', 500); chain(osc(c, 'sine', 1667), env(c, 0.6, 0.012), hp); chain(osc(c, 'triangle', 455), env(c, 0.5, 0.018), hp); hp.connect(out);
  })],
  ['clave', 'Clave', () => render(0.12, (c, out) => chain(osc(c, 'sine', 2500), env(c, 0.7, 0.035), out))],
  ['maracas', 'Maracas', () => render(0.12, (c, out) => chain(noise(c, 0.12), filt(c, 'highpass', 6000), env(c, 0.6, 0.045, 0.018), out))],
  ['tomlo', 'Low Tom', sweep(140, 88, 0.1, 0.45, 0.6)],
  ['tommid', 'Mid Tom', sweep(200, 128, 0.09, 0.36, 0.5)],
  ['tomhi', 'High Tom', sweep(280, 185, 0.08, 0.3, 0.45)],
  ['congalo', 'Low Conga', sweep(230, 200, 0.03, 0.2, 0.3)],
  ['congahi', 'High Conga', sweep(420, 380, 0.03, 0.16, 0.25)],
];

const INST = [
  // Electric-piano style tine at C4 (root 60): FM pair with a fast-decaying modulator.
  ['inst:keys', 'Keys (C4)', () => render(2.6, (c, out) => {
    const f = 261.63, car = osc(c, 'sine', f), mod = osc(c, 'sine', f * 14), mg = c.createGain();
    mg.gain.setValueAtTime(f * 1.4, 0); mg.gain.exponentialRampToValueAtTime(1, 0.35);
    mod.connect(mg); mg.connect(car.frequency);
    chain(car, env(c, 0.8, 2.4, 0.003), out);
    chain(osc(c, 'sine', f * 2), env(c, 0.18, 1.1, 0.003), out);
  })],
  // Long saturated sub at C2 (root 36).
  ['inst:sub', 'Sub 808 (C2)', () => render(1.8, (c, out) => {
    const o = osc(c, 'sine', 65.41), ws = c.createWaveShaper(), cv = new Float32Array(512);
    for (let i = 0; i < 512; i++) cv[i] = Math.tanh((i / 255.5 - 1) * 2.2);
    ws.curve = cv; o.frequency.setValueAtTime(130, 0); o.frequency.exponentialRampToValueAtTime(65.41, 0.04);
    chain(o, env(c, 1, 1.6, 0.002), ws, out);
  })],
];

export async function buildKit() {
  const all = [...KIT.map(([id, name, fn]) => ['808:' + id, name, fn, '808 Kit']), ...INST.map(([id, name, fn]) => [id, name, fn, 'Instruments'])];
  const bufs = await Promise.all(all.map(a => a[2]()));
  return all.map(([id, name, , group], i) => ({ id, name, group, buffer: bufs[i] }));
}
