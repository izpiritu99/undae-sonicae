// MP3 encoding off the main thread with LAME (lamejs, LGPL, loaded as its own file: js/vendor/lame.min.js).
// In: { left, right (Float32Array, right may be null), sampleRate, kbps }. Out: progress messages, then { done: Blob }.
importScripts('../vendor/lame.min.js');
const toInt16 = (f, a, b) => { const o = new Int16Array(b - a); for (let i = a; i < b; i++) { const v = Math.max(-1, Math.min(1, f[i])); o[i - a] = v < 0 ? v * 0x8000 : v * 0x7fff; } return o; };
onmessage = ({ data: { left, right, sampleRate, kbps } }) => {
  try {
    const enc = new lamejs.Mp3Encoder(right ? 2 : 1, sampleRate, kbps), parts = [], N = 1152 * 20;
    for (let i = 0; i < left.length; i += N) {
      const e = Math.min(left.length, i + N), out = right ? enc.encodeBuffer(toInt16(left, i, e), toInt16(right, i, e)) : enc.encodeBuffer(toInt16(left, i, e));
      if (out.length) parts.push(new Uint8Array(out));
      if ((i / N) % 40 === 0) postMessage({ progress: i / left.length });
    }
    const end = enc.flush(); if (end.length) parts.push(new Uint8Array(end));
    postMessage({ done: new Blob(parts, { type: 'audio/mpeg' }) });
  } catch (e) { postMessage({ error: String(e && e.message || e) }); }
};
