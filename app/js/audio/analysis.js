// Audio analysis: pitch tracking (YIN), melody/chord transcription, key detection, tempo estimate.
import { PPQ, NOTES, mtof, SCALES } from '../util.js';

export function toMono(buf) {
  const n = buf.length, C = buf.numberOfChannels, o = new Float32Array(n);
  for (let c = 0; c < C; c++) { const d = buf.getChannelData(c); for (let i = 0; i < n; i++) o[i] += d[i] / C; }
  return o;
}
export function decimate(x, sr, target) {
  const f = Math.max(1, Math.round(sr / target));
  if (f === 1) return { x, sr };
  const n = Math.floor(x.length / f), o = new Float32Array(n);
  for (let i = 0, j = 0; i < n; i++) { let s = 0; for (let k = 0; k < f; k++) s += x[j++]; o[i] = s / f; }
  return { x: o, sr: sr / f };
}
export function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang), half = len >> 1;
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < half; k++) {
        const a = i + k, b = a + half, xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}
const tick = () => new Promise(r => setTimeout(r));
// Short-time magnitude spectra; fn(mag, frameIndex). Yields to the UI periodically.
async function stft(x, N, hop, fn, prog) {
  if (x.length < N) { const p = new Float32Array(N); p.set(x); x = p; }
  const win = new Float32Array(N), re = new Float32Array(N), im = new Float32Array(N), mag = new Float32Array(N / 2);
  for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
  const total = Math.floor((x.length - N) / hop) + 1;
  for (let f = 0; f < total; f++) {
    const p = f * hop;
    for (let i = 0; i < N; i++) { re[i] = x[p + i] * win[i]; im[i] = 0; }
    fft(re, im);
    for (let i = 0; i < N / 2; i++) mag[i] = Math.hypot(re[i], im[i]);
    fn(mag, f);
    if (f % 40 === 39) { prog && prog(f / total); await tick(); }
  }
}

const MAJ = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88], MIN = [6.33, 2.68, 3.52, 5.38, 2.6, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17];
// Krumhansl-Schmuckler: correlate a 12-bin pitch-class histogram against rotated major/minor profiles.
export function keyFromChroma(ch) {
  const res = [], mean = a => a.reduce((s, v) => s + v, 0) / 12, cm = mean(ch);
  for (let root = 0; root < 12; root++) for (const [minor, prof] of [[false, MAJ], [true, MIN]]) {
    const pm = mean(prof); let num = 0, da = 0, dbb = 0;
    for (let i = 0; i < 12; i++) { const a = ch[(i + root) % 12] - cm, b = prof[i] - pm; num += a * b; da += a * a; dbb += b * b; }
    const num12 = (((minor ? root + 3 : root) * 7) % 12 + 7) % 12 + 1;
    res.push({ root, minor, r: da ? num / Math.sqrt(da * dbb) : 0, name: NOTES[root] + (minor ? ' minor' : ' major'), camelot: num12 + (minor ? 'A' : 'B') });
  }
  res.sort((a, b) => b.r - a.r);
  const max = Math.max(...ch) || 1;
  return { ...res[0], alts: res.slice(1, 4), chroma: Array.from(ch, v => v / max), conf: Math.max(0, Math.min(1, (res[0].r - res[1].r) * 4 + res[0].r * 0.5)) };
}
export async function detectKey(xin, srIn, prog) {
  const { x, sr } = decimate(xin, srIn, 11025), N = 4096, chroma = new Float64Array(12), pc = new Int8Array(N / 2).fill(-1);
  for (let i = 1; i < N / 2; i++) { const f = i * sr / N; if (f >= 60 && f <= 2100) pc[i] = ((Math.round(12 * Math.log2(f / 440) + 69) % 12) + 12) % 12; }
  await stft(x, N, 2048, mag => { for (let i = 1; i < N / 2; i++) if (pc[i] >= 0) chroma[pc[i]] += mag[i]; }, prog);
  return keyFromChroma(chroma);
}
export function keyFromNotes(notes) {
  const ch = new Float64Array(12);
  for (const n of notes) ch[n.n % 12] += n.d;
  return keyFromChroma(ch);
}

export function detectBpm(xin, srIn) {
  const { x, sr } = decimate(xin, srIn, 11025), hop = 128, n = Math.floor(x.length / hop), fps = sr / hop;
  if (n < fps * 3) return 0;
  const o = new Float32Array(n); let prev = 0, mean = 0;
  for (let k = 0; k < n; k++) {
    let s = 0; for (let j = 0; j < hop; j++) { const v = x[k * hop + j]; s += v * v; }
    const e = Math.log(1 + 100 * Math.sqrt(s / hop)); o[k] = Math.max(0, e - prev); prev = e; mean += o[k];
  }
  mean /= n; for (let k = 0; k < n; k++) o[k] -= mean;
  const lo = Math.floor(fps * 60 / 200), hi = Math.ceil(fps * 60 / 60), ac = new Float32Array(hi + 2); let best = -1, bl = 0;
  for (let lag = lo - 1; lag <= hi + 1; lag++) {
    let s = 0; for (let k = lag; k < n; k++) s += o[k] * o[k - lag];
    ac[lag] = (s / (n - lag)) * Math.exp(-0.5 * (Math.log2(60 * fps / lag / 120) / 0.8) ** 2);
    if (lag >= lo && lag <= hi && ac[lag] > best) { best = ac[lag]; bl = lag; }
  }
  if (!bl || best <= 0) return 0;
  const a = ac[bl - 1], b = ac[bl], c = ac[bl + 1], den = a - 2 * b + c;
  return Math.round(600 * fps / (bl + (den ? (a - c) / (2 * den) : 0))) / 10;
}

// YIN fundamental estimator on one frame. Returns the period in samples (fractional) or 0 when unvoiced.
export function yin(x, off, W, tMin, tMax, d) {
  let run = 0, best = 0, bv = 1;
  d[0] = 1;
  for (let tau = 1; tau <= tMax; tau++) {
    let s = 0;
    for (let j = 0; j < W; j++) { const q = x[off + j] - x[off + j + tau]; s += q * q; }
    run += s; d[tau] = run ? s * tau / run : 1;
  }
  for (let tau = tMin; tau < tMax; tau++) {
    if (d[tau] < 0.12) { while (tau + 1 < tMax && d[tau + 1] < d[tau]) tau++; best = tau; break; }
    if (d[tau] < bv) { bv = d[tau]; best = tau; }
  }
  if (!best || d[best] > 0.2) return 0;
  const a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2 * b + c;
  return best + (den ? (a - c) / (2 * den) : 0);
}

// Monophonic transcription: frame-wise YIN, median smoothing, then note segmentation. Notes are in seconds.
export async function melodyNotes(xin, srIn, o = {}, prog) {
  const { x, sr } = decimate(xin, srIn, 11025), W = 512, hop = 128;
  const tMin = Math.max(2, Math.floor(sr / 1600)), tMax = Math.ceil(sr / (o.minHz || 38)), d = new Float32Array(tMax + 2);
  const n = Math.max(0, Math.floor((x.length - W - tMax - 1) / hop)), raw = new Float32Array(n), amp = new Float32Array(n);
  let peak = 0;
  for (let k = 0; k < n; k++) { let s = 0; const off = k * hop; for (let j = 0; j < W; j++) s += x[off + j] * x[off + j]; amp[k] = Math.sqrt(s / W); if (amp[k] > peak) peak = amp[k]; }
  const gate = peak * (o.gate ?? 0.08);
  for (let k = 0; k < n; k++) {
    const tau = amp[k] < gate ? 0 : yin(x, k * hop, W, tMin, tMax, d);
    raw[k] = tau ? 69 + 12 * Math.log2(sr / tau / 440) : NaN;
    if (k % 300 === 299) { prog && prog(k / n); await tick(); }
  }
  const pit = new Float32Array(n), w = [];
  for (let k = 0; k < n; k++) {
    w.length = 0;
    for (let j = Math.max(0, k - 2); j <= Math.min(n - 1, k + 2); j++) if (!isNaN(raw[j])) w.push(raw[j]);
    pit[k] = w.length >= 3 ? w.sort((a, b) => a - b)[w.length >> 1] : NaN;
  }
  const hs = hop / sr, minF = Math.max(2, Math.round((o.minDur ?? 0.07) / hs)), notes = [];
  let cur = null, gap = 0;
  const close = () => {
    if (cur && cur.end - cur.start + 1 >= minF) notes.push({ s: cur.start * hs, e: (cur.end + 1) * hs, n: Math.round(cur.sum / cur.cnt), v: Math.min(1, 0.3 + 0.7 * Math.sqrt(cur.pk / peak)) });
    cur = null;
  };
  for (let k = 0; k < n; k++) {
    const m = pit[k];
    if (isNaN(m)) { if (cur && ++gap > 3) close(); continue; }
    const reattack = cur && k - cur.start > 6 && k >= 3 && amp[k] > amp[k - 3] * 1.9 && amp[k - 3] < cur.pk * 0.5;
    if (cur && !reattack && Math.abs(m - cur.sum / cur.cnt) < 0.8) { cur.end = k; cur.sum += m; cur.cnt++; if (amp[k] > cur.pk) cur.pk = amp[k]; gap = 0; }
    else { close(); cur = { start: k, end: k, sum: m, cnt: 1, pk: amp[k] }; gap = 0; }
  }
  close();
  // Clean-up: drop brief octave-jump blips (typical sub-harmonic errors on tails/echoes), then join split notes.
  const far = (a, b) => !b || Math.abs(a.n - b.n) >= 11;
  const kept = notes.filter((n, i) => !(n.e - n.s < 0.16 && far(n, notes[i - 1]) && far(n, notes[i + 1]) && notes.length > 2)), out = [];
  for (const n of kept) { const p = out[out.length - 1]; if (p && p.n === n.n && n.s - p.e < 0.05) { p.e = n.e; p.v = Math.max(p.v, n.v); } else out.push(n); }
  return out;
}

// Polyphonic (experimental): harmonic-sum salience per semitone with iterative cancellation, then frame tracking.
export async function chordNotes(xin, srIn, o = {}, prog) {
  const { x, sr } = decimate(xin, srIn, 22050), N = 4096, hop = 1024, lo = 40, hi = 96, H = 6, maxP = o.maxPoly || 5;
  const bins = [];
  for (let p = lo; p <= hi; p++) { const f = mtof(p), b = []; for (let h = 1; h <= H; h++) { const k = Math.round(f * h * N / sr); if (k < N / 2 - 3) b.push(k); } bins.push(b); }
  const frames = [], firsts = [];
  await stft(x, N, hop, m => {
    const picks = [];
    for (let it = 0; it < maxP; it++) {
      let bp = -1, bs = 0;
      for (let i = 0; i < bins.length; i++) {
        const b = bins[i], f0 = Math.max(m[b[0] - 1], m[b[0]], m[b[0] + 1]); let s = 0;
        for (let h = 0; h < b.length; h++) s += Math.max(m[b[h] - 1], m[b[h]], m[b[h] + 1]) / (h + 1);
        if (f0 > s * 0.22 && s > bs) { bs = s; bp = i; }
      }
      if (bp < 0 || (picks.length && bs < picks[0][1] * 0.3)) break;
      picks.push([bp + lo, bs]);
      for (const k of bins[bp]) for (let j = -2; j <= 2; j++) m[k + j] = 0;
    }
    frames.push(picks); if (picks.length) firsts.push(picks[0][1]);
  }, prog);
  if (!firsts.length) return [];
  firsts.sort((a, b) => a - b);
  const ref = firsts[Math.floor(firsts.length * 0.9)], thr = ref * (o.gate ?? 0.08) * 2.5, hs = hop / sr, on = new Map(), notes = [];
  const close = (p, r) => { if (r.last - r.start >= 1) notes.push({ s: r.start * hs, e: (r.last + 1) * hs, n: p, v: Math.min(1, 0.35 + 0.65 * Math.sqrt(r.sum / r.cnt / ref)) }); };
  frames.forEach((picks, f) => {
    for (const [p, s] of picks) {
      if (s < thr) continue;
      const r = on.get(p);
      if (r && f - r.last <= 2) { r.last = f; r.sum += s; r.cnt++; } else { if (r) close(p, r); on.set(p, { start: f, last: f, sum: s, cnt: 1 }); }
    }
    for (const [p, r] of on) if (f - r.last > 2) { close(p, r); on.delete(p); }
  });
  for (const [p, r] of on) close(p, r);
  return notes.sort((a, b) => a.s - b.s);
}

export function toTicks(notes, bpm, q, align) {
  const tps = bpm / 60 * PPQ, t0 = align && notes.length ? Math.min(...notes.map(n => n.s)) : 0;
  return notes.map(n => {
    let t = (n.s - t0) * tps, d = (n.e - n.s) * tps;
    if (q) { t = Math.round(t / q) * q; d = Math.max(q, Math.round(d / q) * q); }
    return { t: Math.max(0, Math.round(t)), d: Math.max(6, Math.round(d)), n: n.n, v: n.v };
  });
}
export function snapToScale(n, key) {
  const sc = SCALES[key.scale] || SCALES.chromatic;
  for (const d of [0, 1, -1, 2, -2]) if (sc.includes((((n + d - key.root) % 12) + 12) % 12)) return n + d;
  return n;
}
