// End-to-end test of the plug-in host with the test plug-in: node native/test-host.mjs   (after native/build.sh)
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url)), bin = path.join(here, 'bin'), clap = path.join(bin, 'undae-test.clap');
const host = spawn(path.join(bin, 'undae-host'), [], { stdio: ['ignore', 'pipe', 'inherit'] });
const { port, token } = await new Promise((res, rej) => { host.stdout.once('data', d => res(JSON.parse(d))); host.once('exit', c => rej(new Error('host exited ' + c))); });
let failed = 0; const check = (name, ok, extra = '') => { console.log((ok ? 'ok   ' : 'FAIL ') + name + (extra ? '  ' + extra : '')); if (!ok) failed++; };

function connect(tok = token) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/?t=${tok}`); ws.binaryType = 'arraybuffer';
    const waitJson = new Map(), waitAudio = [], events = [], waitEv = []; let seq = 0, pos = 1000;
    ws.onmessage = e => { if (typeof e.data === 'string') { const m = JSON.parse(e.data); if (m.ev) { const i = waitEv.findIndex(w => w.ev === m.ev); if (i >= 0) waitEv.splice(i, 1)[0].res(m); else events.push(m); } else { waitJson.get(m.seq)?.(m); waitJson.delete(m.seq); } } else waitAudio.shift()?.(new Float32Array(e.data, 12)); };
    ws.onerror = () => rej(new Error('refused'));
    ws.onopen = () => res({
      ws, close: () => ws.close(), pos: () => pos,
      event: (ev, ms = 3000) => new Promise(r => { const i = events.findIndex(m => m.ev === ev); if (i >= 0) return r(events.splice(i, 1)[0]); const w = { ev, res: r }; waitEv.push(w); setTimeout(() => { const k = waitEv.indexOf(w); if (k >= 0) { waitEv.splice(k, 1); r(null); } }, ms); }),
      ask: msg => new Promise(r => { const s = ++seq; waitJson.set(s, r); ws.send(JSON.stringify({ ...msg, seq: s })); }),
      tell: msg => ws.send(JSON.stringify(msg)),
      audio: (value, n = 128) => new Promise(r => { const b = new ArrayBuffer(12 + n * 8); new Uint32Array(b, 0, 3).set([1, pos, n]); pos += n; new Float32Array(b, 12).fill(value); waitAudio.push(r); ws.send(b); }),
    });
  });
}
const peak = a => a.reduce((m, v) => Math.max(m, Math.abs(v)), 0), near = (a, b) => Math.abs(a - b) < 1e-4;

try {
  check('wrong token is refused', await connect('nope').then(() => false, () => true));
  const c = await connect();
  const hello = await c.ask({ op: 'hello' }); check('hello', hello.ok && hello.formats.includes('clap'), JSON.stringify(hello));
  check('audio passes through with no plug-in', near(peak(await c.audio(0.5)), 0.5));
  const scan = await c.ask({ op: 'scan', paths: [bin] }); check('scan finds both test plug-ins', scan.ok && ['Undae Test Gain', 'Undae Test Sine'].every(n => scan.plugins.some(p => p.name === n)), scan.plugins.map(p => p.name + (p.instrument ? ' (instrument)' : '')).join(', '));
  const bad = await c.ask({ op: 'load', path: '/nonexistent.clap' }); check('loading a missing file fails cleanly', !bad.ok, bad.error);

  const load = await c.ask({ op: 'load', path: clap, id: 'app.undaesonicae.test-gain', sampleRate: 48000 });
  check('load gain', load.ok && load.params.length === 1 && load.params[0].name === 'Gain' && load.inputs === 2 && load.outputs === 2, JSON.stringify(load.params));
  check('gain 1.0', near(peak(await c.audio(0.5)), 0.5));
  c.tell({ op: 'param', id: 1, value: 0.25 }); await c.audio(0.5);
  check('parameter change reaches the plug-in', near(peak(await c.audio(0.5)), 0.125));
  const saved = await c.ask({ op: 'state' }); check('state saved', saved.ok && saved.data.length > 0, saved.data);
  c.tell({ op: 'param', id: 1, value: 2 }); await c.audio(0.5); check('gain 2.0', near(peak(await c.audio(0.5)), 1));
  const restored = await c.ask({ op: 'state', data: saved.data }); check('state restored', restored.ok && near(restored.params[0].value, 0.25), JSON.stringify(restored.params));
  check('restored gain is audible', near(peak(await c.audio(0.5)), 0.125));
  check('block of 512 frames', (await c.audio(0.5, 512)).length === 1024);

  const d = await connect();                                              // a second instance at the same time, from the same file
  const syn = await d.ask({ op: 'load', path: clap, id: 'app.undaesonicae.test-sine', sampleRate: 48000 });
  check('load synth alongside', syn.ok && syn.notes === true && syn.inputs === 0, `inputs ${syn.inputs}, notes ${syn.notes}`);
  check('synth is silent before a note', peak(await d.audio(0)) === 0);
  d.tell({ op: 'note', on: true, key: 69, vel: 1 }); await d.audio(0); const on = peak(await d.audio(0, 512)); check('note on makes sound', on > 0.15 && on < 0.21, on.toFixed(3));
  d.tell({ op: 'note', on: false, key: 69 }); await d.audio(0); check('note off stops it', peak(await d.audio(0)) === 0);
  d.tell({ op: 'note', on: true, key: 69, vel: 1, at: d.pos() + 128 + 64 }); const b1 = await d.audio(0), b2 = await d.audio(0);   // due 64 frames into the second block
  check('a timed note starts on its frame', peak(b1) === 0 && peak(b2.subarray(0, 126)) === 0 && peak(b2.subarray(130)) > 0.01, `silent before frame 64, peak after ${peak(b2.subarray(130)).toFixed(3)}`);
  d.tell({ op: 'note', on: false, key: 69 }); await d.audio(0);
  {   // controllers: pitch bend, mod wheel, sustain pedal
    const blocks = async n => { const out = []; for (let i = 0; i < n; i++) out.push(...await d.audio(0)); return Float32Array.from(out.filter((_, k) => k % 2 === 0)); };
    const freq = x => { let z = 0; for (let i = 1; i < x.length; i++) if (x[i - 1] < 0 && x[i] >= 0) z++; return z * 48000 / x.length; };
    d.tell({ op: 'note', on: true, key: 69, vel: 1 }); await blocks(2); const f0 = freq(await blocks(16));
    d.tell({ op: 'midi', data: [0xE0, 127, 127] }); await blocks(2); const f1 = freq(await blocks(16));
    check('pitch bend reaches the plug-in', Math.abs(f0 - 440) < 8 && Math.abs(f1 - 440 * 2 ** (2 / 12)) < 8, `${f0.toFixed(0)} Hz -> ${f1.toFixed(0)} Hz (expected 440 -> 494)`);
    d.tell({ op: 'midi', data: [0xE0, 0, 64] }); const a0 = peak(await blocks(4));
    d.tell({ op: 'midi', data: [0xB0, 1, 127] }); await blocks(1); const a1 = peak(await blocks(4));
    check('mod wheel reaches the plug-in', Math.abs(a1 - a0 / 2) < 0.01, `level ${a0.toFixed(3)} -> ${a1.toFixed(3)} (the test synth halves its level at full mod wheel)`);
    d.tell({ op: 'midi', data: [0xB0, 1, 0] });
    d.tell({ op: 'midi', data: [0xB0, 64, 127] }); d.tell({ op: 'note', on: false, key: 69 }); await blocks(2); const held = peak(await blocks(4));
    d.tell({ op: 'midi', data: [0xB0, 64, 0] }); await blocks(1); const after = peak(await blocks(4));
    check('sustain pedal holds a released note', held > 0.15 && after === 0, `with pedal ${held.toFixed(3)}, after lifting it ${after}`);
  }
  check('first instance unaffected', near(peak(await c.audio(0.5)), 0.125));

  if (process.env.DISPLAY) {                                               // the plug-in's own window
    const tree = () => execFileSync('xwininfo', ['-root', '-tree'], { encoding: 'utf8' }).split('\n');
    const find = () => {                                                   // our window has no class; the window manager's frame of the same name does
      const L = tree(), i = L.findIndex(l => /"Undae Test Gain": \(\)/.test(l)); if (i < 0) return null;
      const id = l => (l.match(/0x[0-9a-f]+/) || [])[0], ind = L[i].search(/\S/), k = L.slice(i + 1, i + 4).find(l => /^\s*0x/.test(l) && l.search(/\S/) > ind);
      return { top: id(L[i]), kid: k ? id(k) : null, line: L[i].trim() };
    };
    const pump = async n => { for (let i = 0; i < n; i++) await c.audio(0.5); };
    check('load reports a window', load.gui === true);
    const shown = await c.ask({ op: 'gui', show: true }); check('window opens', shown.ok && shown.open, shown.error || '');
    await new Promise(r => setTimeout(r, 400)); const w = find(); check('window is on screen with the plug-in inside', !!(w && w.kid), w ? w.line : 'not found');
    if (w && w.kid) {
      execFileSync(path.join(bin, 'xpoke'), ['click', w.kid, '85', '70']);       // x = 85 on the bar means gain 0.5
      const ev = (await Promise.all([c.event('param'), (async () => { await new Promise(r => setTimeout(r, 300)); await pump(20); })()]))[0];
      check('a click in the window reaches the host', !!ev && ev.changes[0].id === 1 && near(ev.changes[0].value, 0.5), JSON.stringify(ev));
      check('and changes the sound', near(peak(await c.audio(0.5)), 0.25));
      execFileSync(path.join(bin, 'xpoke'), ['close', w.top]); const closed = await c.event('gui');
      check('closing the window is reported', !!closed && closed.open === false); await new Promise(r => setTimeout(r, 200));
      check('window is gone', !find());
      const again = await c.ask({ op: 'gui', show: true }); await new Promise(r => setTimeout(r, 300)); check('window reopens', again.ok && !!find());
      c.tell({ op: 'param', id: 1, value: 0.25 }); await pump(4);
    }
    const syn2 = await d.ask({ op: 'gui', show: true }); check('a plug-in without a window says so', !syn2.ok, syn2.error);
  } else console.log('skip window checks (no DISPLAY)');

  const t0 = performance.now(); for (let i = 0; i < 2000; i++) await c.audio(0.5); const per = (performance.now() - t0) / 2000;
  check('round trip per 128-frame block', per < 2.6, per.toFixed(3) + ' ms (a block lasts 2.67 ms at 48 kHz)');
  const un = await c.ask({ op: 'unload' }); check('unload (closes its window)', un.ok && near(peak(await c.audio(0.5)), 0.5));
  c.close(); d.close(); await new Promise(r => setTimeout(r, 200));
  check('host survives disconnects', host.exitCode === null);
} catch (e) { check('test run', false, String(e && e.stack || e)); }
host.kill();
console.log(failed ? `${failed} FAILED` : 'all passed');
process.exit(failed ? 1 : 0);
