// Real-time audio bridge to the native plug-in host (see native/PROTOCOL.md).
//
//   AudioWorklet 'host-bridge'  --ring-->  Worker  --WebSocket-->  host process  --WebSocket-->  Worker  --ring-->  AudioWorklet
//
// The worklet never waits: it pushes each render quantum into one shared ring and pulls processed audio from the other.
// `latency` frames are buffered before playback starts, which is the delay the bridge adds. After a dropout the buffer
// grows by 512 frames (up to 4096), so it settles at whatever this computer needs.
// Needs SharedArrayBuffer, so the page must be cross-origin isolated (tools/serve.js sends the headers).

const HEAD = 8;                                      // Int32 header: [write index, read index, underruns, waits, worker ready, first frame, buffer frames, longest audio-thread gap ms]
const MAX_LATENCY = 4096;                            // the buffer grows after a dropout, up to this many frames
export const ringBytes = frames => HEAD * 4 + frames * 2 * 4;

const WORKLET = `
const HEAD = ${HEAD};
const ring = sab => ({ h: new Int32Array(sab, 0, HEAD), d: new Float32Array(sab, HEAD * 4), n: (sab.byteLength - HEAD * 4) / 8 });
class HostBridge extends AudioWorkletProcessor {
  constructor(o) { super(); const p = o.processorOptions; this.o = ring(p.toHost); this.i = ring(p.fromHost); this.lat = p.latency; this.max = p.max || ${MAX_LATENCY}; this.primed = false; Atomics.store(this.i.h, 6, this.lat); }
  process(inputs, outputs) {
    const out = outputs[0], n = out[0].length, L = inputs[0][0], R = inputs[0][1] || L, o = this.o, i = this.i;
    if (!Atomics.load(o.h, 4)) return true;                                           // the connection is not up yet
    const ms = Date.now(); if (this.seen && ms - this.seen > Atomics.load(i.h, 7)) Atomics.store(i.h, 7, ms - this.seen); this.seen = ms;   // how long the audio thread itself was held up
    if (!this.started) { this.started = true; Atomics.store(o.h, 5, currentFrame | 0); }   // which audio frame the stream starts at
    let w = Atomics.load(o.h, 0);
    for (let k = 0; k < n; k++) { const j = ((w + k) % o.n) * 2; o.d[j] = L ? L[k] : 0; o.d[j + 1] = R ? R[k] : 0; }
    Atomics.store(o.h, 0, (w + n) % o.n); Atomics.notify(o.h, 0);
    const r = Atomics.load(i.h, 1); let have = (Atomics.load(i.h, 0) - r + i.n) % i.n;
    if (!this.primed) { if (have < this.lat) return true; this.primed = true; }
    if (have < n) {
      // The buffer ran dry. This happens when the audio thread was held up and is now catching up in a burst: the blocks it
      // just sent have not come back yet. They are only a fraction of a millisecond away, so wait for them here.
      Atomics.add(i.h, 3, 1);
      for (const t0 = Date.now(); have < n && Date.now() - t0 < 3;) have = (Atomics.load(i.h, 0) - r + i.n) % i.n;
      if (have < n) {                                                                    // a real stall: output silence, and buffer more from now on so the next one fits
        Atomics.add(i.h, 2, 1); this.primed = false;
        if (this.lat < this.max) { this.lat = Math.min(this.max, this.lat + 512); Atomics.store(i.h, 6, this.lat); }
        return true;
      }
    }
    const oL = out[0], oR = out[1] || out[0];
    for (let k = 0; k < n; k++) { const j = ((r + k) % i.n) * 2; oL[k] = i.d[j]; if (oR !== oL) oR[k] = i.d[j + 1]; }
    Atomics.store(i.h, 1, (r + n) % i.n);
    return true;
  }
}
registerProcessor('host-bridge', HostBridge);`;

const WORKER = `
const HEAD = ${HEAD};
const ring = sab => ({ h: new Int32Array(sab, 0, HEAD), d: new Float32Array(sab, HEAD * 4), n: (sab.byteLength - HEAD * 4) / 8 });
let o, i, ws, block, open = false, pos = -1;
let last = 0, gap = 0, rt = 0, lastStat = 0; const sent = [];   // timing: longest wait between wake-ups and longest round trip, reported once a second
onmessage = ({ data }) => {
  if (data.op === 'start') {
    o = ring(data.toHost); i = ring(data.fromHost); block = data.block;
    ws = new WebSocket(data.url); ws.binaryType = 'arraybuffer';
    ws.onopen = () => { open = true; Atomics.store(o.h, 4, 1); postMessage({ op: 'open' }); pump(); };
    ws.onclose = () => { open = false; postMessage({ op: 'close' }); };
    ws.onerror = () => postMessage({ op: 'error' });
    ws.onmessage = e => {
      if (typeof e.data === 'string') return postMessage({ op: 'host', msg: JSON.parse(e.data) });
      const head = new Uint32Array(e.data, 0, 3), n = head[2], src = new Float32Array(e.data, 12, n * 2), w = Atomics.load(i.h, 0);
      const t = performance.now(), took = t - sent.shift(); if (took > rt) rt = took;
      if (t - lastStat > 1000) { postMessage({ op: 'stat', gap, rt }); gap = rt = 0; lastStat = t; }
      for (let k = 0; k < n; k++) { const j = ((w + k) % i.n) * 2; i.d[j] = src[k * 2]; i.d[j + 1] = src[k * 2 + 1]; }
      Atomics.store(i.h, 0, (w + n) % i.n);
    };
  } else if (data.op === 'send' && open) ws.send(JSON.stringify(data.msg));
  else if (data.op === 'stop') { open = false; try { ws.close(); } catch {} close(); }
};
async function pump() {
  while (open) {
    const w = Atomics.load(o.h, 0); let r = Atomics.load(o.h, 1);
    const now = performance.now(); if (last && now - last > gap) gap = now - last; last = now;
    while ((w - r + o.n) % o.n >= block) {
      const buf = new ArrayBuffer(12 + block * 8), head = new Uint32Array(buf, 0, 3), dst = new Float32Array(buf, 12);
      if (pos < 0) pos = Atomics.load(o.h, 5) >>> 0;
      head[0] = 1; head[1] = pos; head[2] = block; pos = (pos + block) >>> 0;        // each block carries the audio frame it starts at
      for (let k = 0; k < block; k++) { const j = ((r + k) % o.n) * 2; dst[k * 2] = o.d[j]; dst[k * 2 + 1] = o.d[j + 1]; }
      r = (r + block) % o.n; Atomics.store(o.h, 1, r); sent.push(now); ws.send(buf);
    }
    const wait = Atomics.waitAsync(o.h, 0, w); if (wait.async) await wait.value;
  }
}`;

// Without SharedArrayBuffer (the Android app's WebView is not cross-origin isolated) the same stream goes through message
// ports instead: the worklet posts each block straight to the worker over a MessageChannel and gets processed blocks back
// the same way. A little more overhead per block, which is why remote hosts use bigger blocks.
const PORT_WORKLET = `
class HostBridgePort extends AudioWorkletProcessor {
  constructor(o) {
    super(); const p = o.processorOptions; this.block = p.block; this.lat = p.latency; this.max = p.max;
    this.acc = new Float32Array(p.block * 2); this.accN = 0; this.q = []; this.qOff = 0; this.have = 0; this.primed = false; this.under = 0; this.started = false;
    this.port.onmessage = e => {
      if (!e.data.link) return;
      this.link = e.data.link;
      this.link.onmessage = m => { const d = m.data; if (d.open) { this.open = true; return; } this.q.push(d); this.have += d.length / 2; };
    };
  }
  process(inputs, outputs) {
    const out = outputs[0], n = out[0].length, L = inputs[0][0], R = inputs[0][1] || L;
    if (!this.link || !this.open) return true;
    if (!this.started) { this.started = true; this.link.postMessage({ first: currentFrame }); }
    for (let k = 0; k < n; k++) {
      this.acc[this.accN * 2] = L ? L[k] : 0; this.acc[this.accN * 2 + 1] = R ? R[k] : 0;
      if (++this.accN === this.block) { this.link.postMessage(this.acc, [this.acc.buffer]); this.acc = new Float32Array(this.block * 2); this.accN = 0; }
    }
    // a burst after a network hold-up: drop the oldest audio so the delay does not keep growing
    if (this.have > this.lat * 2 + 4096) { let drop = this.have - this.lat; while (drop > 0 && this.q.length) { const b = this.q[0], left = b.length / 2 - this.qOff; if (left <= drop) { drop -= left; this.have -= left; this.q.shift(); this.qOff = 0; } else { this.qOff += drop; this.have -= drop; drop = 0; } } }
    if (!this.primed) { if (this.have < this.lat) return true; this.primed = true; }
    if (this.have < n) {
      this.under++; this.primed = false;
      if (this.lat < this.max) this.lat = Math.min(this.max, this.lat + 512);
      this.port.postMessage({ lat: this.lat, under: this.under }); return true;
    }
    const oL = out[0], oR = out[1] || out[0]; let k = 0;
    while (k < n) {
      const b = this.q[0], take = Math.min(b.length / 2 - this.qOff, n - k);
      for (let j = 0; j < take; j++) { oL[k + j] = b[(this.qOff + j) * 2]; if (oR !== oL) oR[k + j] = b[(this.qOff + j) * 2 + 1]; }
      k += take; this.qOff += take; if (this.qOff * 2 >= b.length) { this.q.shift(); this.qOff = 0; }
    }
    this.have -= n; return true;
  }
}
registerProcessor('host-bridge-port', HostBridgePort);`;

const PORT_WORKER = `
let ws, link, block, pos = -1, open = false;
onmessage = ({ data }) => {
  if (data.op === 'start') {
    link = data.link; block = data.block;
    link.onmessage = m => {
      const d = m.data; if (d.first != null) { pos = d.first >>> 0; return; }
      if (!open || pos < 0) return;
      const buf = new ArrayBuffer(12 + d.length * 4); new Uint32Array(buf, 0, 3).set([1, pos, d.length / 2]); new Float32Array(buf, 12).set(d);
      pos = (pos + d.length / 2) >>> 0; ws.send(buf);
    };
    ws = new WebSocket(data.url); ws.binaryType = 'arraybuffer';
    ws.onopen = () => { open = true; link.postMessage({ open: true }); postMessage({ op: 'open' }); };
    ws.onclose = () => { open = false; postMessage({ op: 'close' }); };
    ws.onerror = () => postMessage({ op: 'error' });
    ws.onmessage = e => {
      if (typeof e.data === 'string') return postMessage({ op: 'host', msg: JSON.parse(e.data) });
      const f = new Float32Array(e.data.slice(12)); link.postMessage(f, [f.buffer]);
    };
  } else if (data.op === 'send' && open) ws.send(JSON.stringify(data.msg));
  else if (data.op === 'stop') { open = false; try { ws.close(); } catch {} close(); }
};`;

const blobUrl = src => URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
const sabOk = () => typeof SharedArrayBuffer !== 'undefined' && globalThis.crossOriginIsolated === true;
export const bridgeAvailable = () => typeof AudioWorkletNode !== 'undefined';

// Returns { node, send(msg), onhost, stats(), close() }. Connect audio through `node` like any other AudioNode.
export const bridgeDefaults = { latency: 1024 };   // frames buffered before playback; raise it if the host cannot keep up
// Remote hosts (a plug-in server on another computer) use bigger blocks and may buffer much more: Wi-Fi is uneven.
export async function createBridge(ctx, { url, id = 1, latency = bridgeDefaults.latency, block, ringFrames, max } = {}) {
  const remote = isRemoteUrl(url);
  block ||= remote ? 256 : 128; max ||= remote ? 16384 : MAX_LATENCY; ringFrames ||= remote ? 65536 : 8192;
  if (!bridgeAvailable()) throw new Error('This browser cannot run plug-ins (no AudioWorklet)');
  if (!sabOk()) return createPortBridge(ctx, { url, latency, block, max });
  ctx.__bridge ||= ctx.audioWorklet.addModule(blobUrl(WORKLET)); await ctx.__bridge;
  const toHost = new SharedArrayBuffer(ringBytes(ringFrames)), fromHost = new SharedArrayBuffer(ringBytes(ringFrames));
  const node = new AudioWorkletNode(ctx, 'host-bridge', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit', processorOptions: { toHost, fromHost, latency, max } });
  const head = new Int32Array(fromHost, 0, HEAD);
  const worker = new Worker(blobUrl(WORKER)), api = { node, get latency() { return Atomics.load(head, 6) || latency; }, onhost: null, state: 'connecting', timing: { gap: 0, rt: 0 } }, waiting = new Map(); let seq = 0;
  const ready = new Promise((res, rej) => {
    worker.onmessage = ({ data }) => {
      if (data.op === 'open') { api.state = 'open'; res(); }
      else if (data.op === 'close' || data.op === 'error') { const first = api.state === 'connecting'; api.state = 'closed'; if (first) rej(new Error('The plug-in host is not reachable')); }
      else if (data.op === 'stat') { api.timing.gap = Math.max(api.timing.gap, data.gap); api.timing.rt = Math.max(api.timing.rt, data.rt); }
      else if (data.op === 'host') { const m = data.msg, w = m.seq && waiting.get(m.seq); if (w) { waiting.delete(m.seq); w(m); } else if (api.onhost) api.onhost(m); }
    };
  });
  worker.postMessage({ op: 'start', toHost, fromHost, url, id, block });
  api.send = msg => worker.postMessage({ op: 'send', msg });
  api.ask = msg => new Promise(res => { const s = ++seq; waiting.set(s, res); api.send({ ...msg, seq: s }); setTimeout(() => { if (waiting.delete(s)) res({ ok: false, error: 'The plug-in host did not answer' }); }, 15000); });
  // The frame to stamp on a note so that it is heard at context time t: the bridge delays audio by its buffer plus about a block.
  api.frameAt = t => (Math.round(t * ctx.sampleRate) - api.latency - block) >>> 0;
  api.stats = () => ({ state: api.state, underruns: Atomics.load(head, 2), waits: Atomics.load(head, 3), latency: api.latency, audioGapMs: Atomics.exchange(head, 7, 0) });   // underruns: audible gaps; waits: the audio thread paused for the host
  api.close = () => { worker.postMessage({ op: 'stop' }); try { node.disconnect(); } catch { /* not connected */ } };
  await ready;
  return api;
}

async function createPortBridge(ctx, { url, latency, block, max }) {
  ctx.__bridgePort ||= ctx.audioWorklet.addModule(blobUrl(PORT_WORKLET)); await ctx.__bridgePort;
  const node = new AudioWorkletNode(ctx, 'host-bridge-port', { numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit', processorOptions: { latency, block, max } });
  const ch = new MessageChannel(), worker = new Worker(blobUrl(PORT_WORKER)), waiting = new Map(); let seq = 0, lat = latency, under = 0;
  node.port.onmessage = ({ data }) => { if (data.lat) { lat = data.lat; under = data.under; } };
  node.port.postMessage({ link: ch.port1 }, [ch.port1]);
  const api = { node, get latency() { return lat; }, onhost: null, state: 'connecting', timing: { gap: 0, rt: 0 } };
  const ready = new Promise((res, rej) => {
    worker.onmessage = ({ data }) => {
      if (data.op === 'open') { api.state = 'open'; res(); }
      else if (data.op === 'close' || data.op === 'error') { const first = api.state === 'connecting'; api.state = 'closed'; if (first) rej(new Error('The plug-in host is not reachable')); }
      else if (data.op === 'host') { const m = data.msg, w = m.seq && waiting.get(m.seq); if (w) { waiting.delete(m.seq); w(m); } else if (api.onhost) api.onhost(m); }
    };
  });
  worker.postMessage({ op: 'start', url, block, link: ch.port2 }, [ch.port2]);
  api.send = msg => worker.postMessage({ op: 'send', msg });
  api.ask = msg => new Promise(res => { const s = ++seq; waiting.set(s, res); api.send({ ...msg, seq: s }); setTimeout(() => { if (waiting.delete(s)) res({ ok: false, error: 'The plug-in host did not answer' }); }, 15000); });
  api.frameAt = t => (Math.round(t * ctx.sampleRate) - lat - block) >>> 0;
  api.stats = () => ({ state: api.state, underruns: under, waits: 0, latency: lat, audioGapMs: 0 });
  api.close = () => { worker.postMessage({ op: 'stop' }); try { node.disconnect(); } catch { /* not connected */ } };
  await ready;
  return api;
}

// A plug-in server: another computer running the desktop app with sharing on (tools/share.js). { addr: 'host:port',
// code, on }. While one is set and switched on, every plug-in goes through it instead of this computer's own hosts.
export const SHARE_PORT = 47800;
let server = null;
const serverBase = s => { let a = String(s.addr || '').trim().replace(/^[a-z]+:\/\//i, '').replace(/\/.*$/, ''); if (a && !/:\d+$/.test(a)) a += ':' + SHARE_PORT; return a; };
const isRemoteUrl = url => !!url && !/^ws:\/\/(127\.0\.0\.1|localhost)[:/]/.test(url);
export function setPlugServer(cfg) { server = cfg && cfg.on && cfg.addr ? { addr: serverBase(cfg), code: String(cfg.code || '').trim() } : null; info = null; }
export const plugServer = () => server;
// Ask a server what it offers. { ok, name, clap, win, win32, bundled, winPending } or { ok: false, error }.
export async function askServer(cfg) {
  const s = { addr: serverBase(cfg), code: String(cfg.code || '').trim() };
  if (!s.addr) return { ok: false, error: 'Enter the computer’s address' };
  try {
    const ctl = new AbortController(), t = setTimeout(() => ctl.abort(), 5000);
    const r = await fetch(`http://${s.addr}/share.json?code=${encodeURIComponent(s.code)}`, { cache: 'no-store', signal: ctl.signal }); clearTimeout(t);
    if (r.status === 403) return { ok: false, error: 'Wrong pairing code' };
    if (r.status === 429) return { ok: false, error: 'Too many wrong codes: wait a minute' };
    if (!r.ok) return { ok: false, error: 'That computer answered, but not as a plug-in server' };
    return await r.json();
  } catch { return { ok: false, error: 'No answer from ' + s.addr + '. Is sharing on there, and are both on the same network?' }; }
}

// Where the plug-in hosts are. The app's own server answers /host.json (see tools/host.js). Result: { url, bundled } for
// the native CLAP host and win: { url, folder } for the Windows VST host under Wine; either may be missing.
let info;
const wsUrl = h => 'ws://127.0.0.1:' + h.port + '/?t=' + h.token;
export function hostInfo() {
  if (server) {
    const s = server, ws = name => `ws://${s.addr}/${name}?code=${encodeURIComponent(s.code)}`;
    return (info ||= askServer(s).then(j => {
      if (!j.ok) { setTimeout(() => { if (server === s) info = null; }, 3000); return null; }
      if (j.winPending) setTimeout(() => { info = null; }, 1500);
      return { remote: true, name: j.name, bundled: j.bundled, url: j.clap ? ws('clap') : null, win: j.win ? { url: ws('win'), folder: j.win.folder } : null, win32: j.win32 ? { url: ws('win32'), folder: j.win32.folder } : null, winPending: !!j.winPending };
    }));
  }
  return (info ||= fetch('host.json', { cache: 'no-store' }).then(r => (r.ok ? r.json() : null)).then(j => {
    if (!j) return null;
    if (j.winPending) setTimeout(() => { info = null; }, 1500);          // Wine is still starting: ask again next time
    const out = { bundled: j.bundled, url: j.port ? wsUrl(j) : null, win: j.win ? { url: wsUrl(j.win), folder: j.win.folder } : null, win32: j.win32 ? { url: wsUrl(j.win32), folder: j.win32.folder } : null, winPending: !!j.winPending };
    return out.url || out.win || out.win32 || out.winPending ? out : null;
  }).catch(() => null));
}
// fmt: 'clap', 'vst2' or 'vst3'; bits: 32 for a 32-bit Windows plug-in (its own host process).
// windows: a Windows plug-in (through Wine). VST3 exists for both; older slots have no flag, so their path decides.
export const isWindowsSlot = s => !!s && (s.fmt === 'vst2' || (s.fmt === 'vst3' && (s.windows ?? /\/\.vst-win\//.test(s.path || ''))));
export const hostUrl = async (fmt, bits, windows) => { const hi = await hostInfo(); if (!hi) return null; if (fmt === 'clap' || !fmt || (fmt === 'vst3' && !windows)) return hi.url; const w = bits === 32 ? hi.win32 : hi.win; return w && w.url; };

// One request without an audio connection (scanning for plug-ins).
export async function hostCall(msg, fmt = 'clap', timeout = 30000, bits, windows = fmt === 'vst2') {
  const url = await hostUrl(fmt, bits, windows); if (!url) return { ok: false, error: 'The plug-in host is not running' };
  return new Promise(res => {
    const ws = new WebSocket(url), done = r => { clearTimeout(t); try { ws.close(); } catch { /* closed */ } res(r); }, t = setTimeout(() => done({ ok: false, error: 'The plug-in host did not answer' }), timeout);
    ws.onopen = () => ws.send(JSON.stringify({ ...msg, seq: 1 }));
    ws.onmessage = e => { if (typeof e.data === 'string') { const m = JSON.parse(e.data); if (m.seq === 1) done(m); } };
    ws.onerror = () => done({ ok: false, error: 'The plug-in host is not reachable' });
  });
}

// Offline rendering for export: run `length` frames through a plug-in as fast as the host answers, with no audio thread
// involved. slot: { path, pid, fmt, state, v<id>... }. input: [left, right] Float32Arrays or null (instrument).
// events: notes { frame, on, key, vel }, parameter changes { frame, id, value } and raw MIDI { frame, data }, in frame order. Returns [left, right].
export async function renderOffline(slot, sampleRate, length, input, notes = []) {
  const url = await hostUrl(slot.fmt, slot.bits, isWindowsSlot(slot)); if (!url) throw new Error('the plug-in host is not running');
  const ws = new WebSocket(url); ws.binaryType = 'arraybuffer';
  const waitJson = new Map(), waitAudio = []; let seq = 0, dead = null;
  const fail = why => { dead = new Error(why); for (const w of [...waitJson.values(), ...waitAudio]) w(null); waitJson.clear(); waitAudio.length = 0; };
  ws.onmessage = e => { if (typeof e.data !== 'string') return waitAudio.shift()?.(new Float32Array(e.data, 12)); const m = JSON.parse(e.data), w = m.seq && waitJson.get(m.seq); if (w) { waitJson.delete(m.seq); w(m); } };
  ws.onclose = () => fail('the plug-in host went away');
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error('the plug-in host is not reachable')); });
  const ask = msg => new Promise(res => { const s = ++seq; waitJson.set(s, res); ws.send(JSON.stringify({ ...msg, seq: s })); });
  try {
    const load = await ask({ op: 'load', path: slot.path, id: slot.pid || '', sampleRate });
    if (!load || !load.ok) throw new Error((load && load.error) || 'the plug-in could not be loaded');
    if (slot.state) await ask({ op: 'state', data: slot.state });
    for (const q of load.params) if (typeof slot['v' + q.id] === 'number') ws.send(JSON.stringify({ op: 'param', id: q.id, value: slot['v' + q.id] }));
    const N = 1024, L = new Float32Array(length), R = new Float32Array(length); let ni = 0;
    for (let pos = 0; pos < length; pos += N) {
      const n = Math.min(N, length - pos), buf = new ArrayBuffer(12 + n * 8), f = new Float32Array(buf, 12);
      for (; ni < notes.length && notes[ni].frame < pos + n; ni++) { const e = notes[ni]; ws.send(JSON.stringify(e.data ? { op: 'midi', data: e.data, at: e.frame } : e.id != null ? { op: 'param', id: e.id, value: e.value, at: e.frame } : { op: 'note', on: e.on, key: e.key, vel: e.vel, at: e.frame })); }
      new Uint32Array(buf, 0, 3).set([1, pos, n]);
      if (input) for (let k = 0; k < n; k++) { f[k * 2] = input[0][pos + k]; f[k * 2 + 1] = input[1][pos + k]; }
      const reply = await new Promise(res => { waitAudio.push(res); ws.send(buf); });
      if (!reply) throw dead || new Error('the plug-in host went away');
      for (let k = 0; k < n; k++) { L[pos + k] = reply[k * 2]; R[pos + k] = reply[k * 2 + 1]; }
    }
    return [L, R];
  } finally { ws.onclose = null; try { ws.close(); } catch { /* closed */ } }
}
