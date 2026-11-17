// Sample pool: built-in kit plus user audio persisted in IndexedDB.
import { uid, bus } from './util.js';
import { prefs } from './state.js';

export const samples = new Map(); // id -> { id, name, buffer, builtin?, group? }
let ctx, db;
const open = () => (db ||= new Promise((res, rej) => {
  const r = indexedDB.open('undae-sonicae', 2);
  r.onupgradeneeded = () => { for (const n of ['samples', 'folders']) if (!r.result.objectStoreNames.contains(n)) r.result.createObjectStore(n, { keyPath: 'id' }); };
  r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error);
}));
const tx = async (mode, fn, name = 'samples') => {
  const d = await open();
  return new Promise((res, rej) => { const t = d.transaction(name, mode), rq = fn(t.objectStore(name)); t.oncomplete = () => res(rq && rq.result); t.onerror = () => rej(t.error); });
};
export const records = () => tx('readonly', s => s.getAll()).catch(() => []);

export async function initSamples(c, builtins) {
  ctx = c;
  for (const b of builtins) samples.set(b.id, { ...b, builtin: true });
  for (const r of await records()) {
    try { samples.set(r.id, { id: r.id, name: r.name, buffer: await ctx.decodeAudioData(r.bytes.slice(0)) }); } catch { /* undecodable, skip */ }
  }
}
export async function addBytes(name, bytes, id = 'u:' + uid()) {
  const buffer = await ctx.decodeAudioData(bytes.slice(0));
  samples.set(id, { id, name, buffer });
  try { await tx('readwrite', s => s.put({ id, name, bytes })); } catch { /* not persisted */ }
  bus.emit('samples');
  return id;
}
export const addFile = async f => addBytes(f.name.replace(/\.[^.]+$/, ''), await f.arrayBuffer());
export const addBuffer = (name, buffer) => addBytes(name, wavBytes(buffer));
export async function removeSample(id) {
  samples.delete(id);
  try { await tx('readwrite', s => s.delete(id)); } catch { /* ignore */ }
  bus.emit('samples');
}

// --- drag and drop ---------------------------------------------------------------
// What is being dragged inside the app (a library sample id, or a file in a sample folder that is imported on drop).
export const drag = { id: null, file: null };
// Resolve a drop to sample ids: an in-app drag, or audio files dragged in from the computer.
export async function dropped(e) {
  const files = [...(e.dataTransfer ? e.dataTransfer.files : [])], d = { ...drag }, out = [];
  drag.id = drag.file = null;
  if (d.id) out.push(d.id);
  else if (d.file) { try { out.push(await addFile(await d.file())); } catch { /* undecodable */ } }
  else for (const f of files) { try { out.push(await addFile(f)); } catch { /* not audio */ } }
  return out;
}

// --- folders on disk: sample locations to browse, and plug-in folders to list --------------
export const folders = [];   // { id, name, kind: 'samples' | 'plugins', handle?, files: [{ name, path, file }], ready }
const AUDIO = /\.(wav|mp3|ogg|oga|flac|aif|aiff|m4a|aac|opus|webm)$/i, PLUG = /\.(vst3|vst|dll|so|clap|component|lv2)$/i;
async function scan(dir, kind, path = '', out = [], depth = 0) {
  for await (const [name, hnd] of dir.entries()) {
    if (out.length >= 3000) break;
    const plug = PLUG.test(name);
    if (hnd.kind === 'directory' && !(kind === 'plugins' && plug)) { if (depth < 5) await scan(hnd, kind, path + name + '/', out, depth + 1); }
    else if (kind === 'plugins' ? plug : AUDIO.test(name)) out.push({ name, path: path + name, file: hnd.kind === 'file' ? () => hnd.getFile() : null });
  }
  return out;
}
export const canKeepFolders = () => 'showDirectoryPicker' in window;
export async function addFolder(kind) {
  if (canKeepFolders()) {
    let handle; try { handle = await window.showDirectoryPicker({ mode: 'read' }); } catch { return null; }
    const f = { id: uid(), name: handle.name, kind, handle, files: await scan(handle, kind), ready: true };
    folders.push(f);
    try { await tx('readwrite', s => s.put({ id: f.id, name: f.name, kind, handle }), 'folders'); } catch { /* not persisted */ }
    bus.emit('folders'); return f;
  }
  // No folder access here (phones, some browsers): pick a folder or a batch of files once, for this session.
  const files = await new Promise(res => { const i = document.createElement('input'); i.type = 'file'; i.multiple = true; i.webkitdirectory = true; i.onchange = () => res([...i.files]); i.click(); });
  if (!files.length) return null;
  const rx = kind === 'plugins' ? PLUG : AUDIO;
  const f = { id: uid(), name: (files[0].webkitRelativePath || '').split('/')[0] || 'Picked files', kind, temp: true, ready: true, files: files.filter(x => rx.test(x.name)).map(x => ({ name: x.name, path: x.webkitRelativePath || x.name, file: async () => x })) };
  folders.push(f); bus.emit('folders'); return f;
}
// Remembered folders come back at startup; the browser may want one click to re-allow reading them.
export async function loadFolders() {
  try { for (const r of await tx('readonly', s => s.getAll(), 'folders')) folders.push({ ...r, files: [], ready: false }); } catch { /* none */ }
  for (const f of folders) { try { if (f.handle && (await f.handle.queryPermission({ mode: 'read' })) === 'granted') { f.files = await scan(f.handle, f.kind); f.ready = true; } } catch { /* stays locked */ } }
  bus.emit('folders');
}
export async function openFolder(f) {
  try { if ((await f.handle.requestPermission({ mode: 'read' })) !== 'granted') return false; f.files = await scan(f.handle, f.kind); f.ready = true; bus.emit('folders'); return true; } catch { return false; }
}
export async function removeFolder(f) {
  folders.splice(folders.indexOf(f), 1);
  try { await tx('readwrite', s => s.delete(f.id), 'folders'); } catch { /* ignore */ }
  bus.emit('folders');
}

// 16-bit PCM WAV encoder.
// bits: 16 or 24 (PCM) or 32 (floating point). Defaults to the quality chosen in Settings > Audio.
export function wavBytes(buf, bits = prefs.bitDepth || 16) {
  const ch = buf.numberOfChannels, n = buf.length, bps = bits / 8, size = n * ch * bps, out = new DataView(new ArrayBuffer(44 + size));
  const str = (o, s) => { for (let i = 0; i < s.length; i++) out.setUint8(o + i, s.charCodeAt(i)); };
  str(0, 'RIFF'); out.setUint32(4, 36 + size, true); str(8, 'WAVEfmt '); out.setUint32(16, 16, true); out.setUint16(20, bits === 32 ? 3 : 1, true); out.setUint16(22, ch, true);
  out.setUint32(24, buf.sampleRate, true); out.setUint32(28, buf.sampleRate * ch * bps, true); out.setUint16(32, ch * bps, true); out.setUint16(34, bits, true);
  str(36, 'data'); out.setUint32(40, size, true);
  const data = []; for (let c = 0; c < ch; c++) data.push(buf.getChannelData(c));
  for (let i = 0, o = 44; i < n; i++) for (let c = 0; c < ch; c++, o += bps) {
    const s = Math.max(-1, Math.min(1, data[c][i]));
    if (bits === 32) out.setFloat32(o, s, true);
    else if (bits === 24) { const v = Math.round(s * 8388607); out.setUint8(o, v & 255); out.setUint8(o + 1, (v >> 8) & 255); out.setUint8(o + 2, (v >> 16) & 255); }
    else out.setInt16(o, s < 0 ? s * 32768 : s * 32767, true);
  }
  return out.buffer;
}
