// Compressed export: an AudioBuffer to an Ogg Opus file, using the browser's own Opus encoder (WebCodecs AudioEncoder)
// faster than real time, with a small Ogg page writer around it.
const CRC = (() => { const t = new Uint32Array(256); for (let i = 0; i < 256; i++) { let r = i << 24; for (let k = 0; k < 8; k++) r = r & 0x80000000 ? (r << 1) ^ 0x04c11db7 : r << 1; t[i] = r >>> 0; } return t; })();
const crc = d => { let c = 0; for (const b of d) c = ((c << 8) ^ CRC[((c >>> 24) ^ b) & 255]) >>> 0; return c; };

function page(packets, granule, serial, seq, flags) {   // one Ogg page holding whole packets
  const lace = []; for (const p of packets) { let n = p.length; while (n >= 255) { lace.push(255); n -= 255; } lace.push(n); }
  const body = packets.reduce((n, p) => n + p.length, 0), out = new Uint8Array(27 + lace.length + body), v = new DataView(out.buffer);
  out.set([79, 103, 103, 83]); out[5] = flags; v.setBigInt64(6, BigInt(granule), true); v.setUint32(14, serial, true); v.setUint32(18, seq, true);
  out[26] = lace.length; out.set(lace, 27); let at = 27 + lace.length; for (const p of packets) { out.set(p, at); at += p.length; }
  v.setUint32(22, crc(out), true); return out;
}

export async function encodeOpus(buf, bitrate = 192000) {
  if (typeof AudioEncoder === 'undefined') throw new Error('This version has no Opus encoder');
  const sr = buf.sampleRate, ch = Math.min(2, buf.numberOfChannels), chunks = [];
  const enc = new AudioEncoder({ output: c => { const d = new Uint8Array(c.byteLength); c.copyTo(d); chunks.push({ d, dur: c.duration }); }, error: e => { throw e; } });
  const cfg = { codec: 'opus', sampleRate: sr, numberOfChannels: ch, bitrate };
  if (!(await AudioEncoder.isConfigSupported(cfg)).supported) throw new Error('Opus encoding is not supported at ' + sr + ' Hz');
  enc.configure(cfg);
  const N = 4800;
  for (let i = 0; i < buf.length; i += N) {
    const n = Math.min(N, buf.length - i), data = new Float32Array(n * ch);
    for (let c = 0; c < ch; c++) data.set(buf.getChannelData(c).subarray(i, i + n), c * n);
    enc.encode(new AudioData({ format: 'f32-planar', sampleRate: sr, numberOfFrames: n, numberOfChannels: ch, timestamp: Math.round(i / sr * 1e6), data }));
  }
  await enc.flush(); enc.close();
  const serial = (Math.random() * 2 ** 32) >>> 0, pre = 312, parts = [];
  const head = new Uint8Array(19), hv = new DataView(head.buffer);
  head.set([79, 112, 117, 115, 72, 101, 97, 100, 1, ch]); hv.setUint16(10, pre, true); hv.setUint32(12, sr, true);
  const vendor = new TextEncoder().encode('Undae Sonicae'), tags = new Uint8Array(8 + 4 + vendor.length + 4), tv = new DataView(tags.buffer);
  tags.set(new TextEncoder().encode('OpusTags')); tv.setUint32(8, vendor.length, true); tags.set(vendor, 12);
  let seq = 0; parts.push(page([head], 0, serial, seq++, 2), page([tags], 0, serial, seq++, 0));
  let gran = pre, batch = [];
  for (let i = 0; i < chunks.length; i++) {
    gran += Math.round(chunks[i].dur * 48000 / 1e6); batch.push(chunks[i].d);
    const last = i === chunks.length - 1;
    if (batch.length === 50 || last) { parts.push(page(batch, last ? pre + Math.round(buf.length * 48000 / sr) : gran, serial, seq++, last ? 4 : 0)); batch = []; }
  }
  return new Blob(parts, { type: 'audio/ogg' });
}

// MP3 (MPEG-1 Layer III, constant bitrate) through LAME in a worker. MP3 only allows 32, 44.1 and 48 kHz here, so other
// rates are resampled to 48 kHz (or 44.1 kHz for 88.2 kHz) first.
export async function encodeMp3(buf, kbps = 320, progress) {
  let b = buf;
  if (![32000, 44100, 48000].includes(b.sampleRate)) {
    const to = b.sampleRate % 44100 === 0 ? 44100 : 48000, oc = new OfflineAudioContext(Math.min(2, b.numberOfChannels), Math.ceil(b.length * to / b.sampleRate), to);
    const s = oc.createBufferSource(); s.buffer = b; s.connect(oc.destination); s.start(); b = await oc.startRendering();
  }
  const left = b.getChannelData(0).slice(), right = b.numberOfChannels > 1 ? b.getChannelData(1).slice() : null;
  const w = new Worker(new URL('./mp3worker.js', import.meta.url));
  try {
    return await new Promise((ok, bad) => {
      w.onmessage = ({ data }) => { if (data.done) ok(data.done); else if (data.error) bad(new Error('MP3 encoding failed: ' + data.error)); else if (progress) progress(data.progress); };
      w.onerror = e => bad(new Error('MP3 encoder could not start: ' + (e.message || 'unknown')));
      w.postMessage({ left, right, sampleRate: b.sampleRate, kbps }, [left.buffer, ...(right ? [right.buffer] : [])]);
    });
  } finally { w.terminate(); }
}
