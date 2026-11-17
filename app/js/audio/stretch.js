// Pitch-preserving time-stretch (WSOLA): overlap-add of windowed grains, each nudged to the spot
// where it lines up best with the audio that came before, so the tempo changes but the pitch does not.
export function stretch(ctx, buf, ratio) {
  const sr = buf.sampleRate, N = sr > 60000 ? 4096 : 2048, H = N >> 1, D = N >> 2, nIn = buf.length, ch = buf.numberOfChannels;
  const nOut = Math.max(1, Math.floor(nIn * ratio)), out = ctx.createBuffer(ch, nOut, sr);
  if (nIn < N * 2) { for (let c = 0; c < ch; c++) { const s = buf.getChannelData(c), d = out.getChannelData(c); for (let i = 0; i < nOut; i++) d[i] = s[Math.min(nIn - 1, Math.floor(i / ratio))]; } return out; }
  let mono = buf.getChannelData(0);
  if (ch > 1) { const r = buf.getChannelData(1); mono = Float32Array.from(mono, (v, i) => (v + r[i]) * 0.5); }
  const win = new Float32Array(N); for (let i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N);
  const frames = Math.ceil(nOut / H), pos = new Int32Array(frames), last = nIn - N;
  for (let k = 1, prev = 0; k < frames; k++) {
    const ideal = Math.min(last, Math.round(k * H / ratio)), want = prev + H;   // 'want' continues the previous grain seamlessly
    let best = ideal;
    if (want <= last) {
      const lo = Math.max(0, ideal - D), hi = Math.min(last, ideal + D); let bs = -Infinity;
      for (let p = lo; p <= hi; p += 2) { let s = 0; for (let i = 0; i < H; i += 8) s += mono[p + i] * mono[want + i]; if (s > bs) { bs = s; best = p; } }
    }
    pos[k] = prev = best;
  }
  for (let c = 0; c < ch; c++) {
    const s = buf.getChannelData(c), d = out.getChannelData(c);
    for (let k = 0; k < frames; k++) { const o = k * H, p = pos[k], n = Math.min(N, nOut - o); for (let i = 0; i < n; i++) d[o + i] += s[p + i] * win[i]; }
    for (let i = 0; i < H && i < nOut; i++) d[i] = s[i];   // the first half-grain has no partner to overlap with
  }
  return out;
}
