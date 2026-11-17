// End-to-end test of the Windows VST host under Wine: node native/test-vsthost.mjs [--32] <folder with plug-ins>...
// --32 tests the 32-bit host (undae-vsthost32.exe), which only lists 32-bit plug-ins.
// With no folders it only checks that the host starts and answers. Needs Wine and native/build.sh.
import { spawn, execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import os from 'node:os';

const here = path.dirname(fileURLToPath(import.meta.url)), b32 = process.argv.includes('--32'), exe = path.join(here, 'bin', b32 ? 'undae-vsthost32.exe' : 'undae-vsthost.exe'), folders = process.argv.slice(2).filter(a => a !== '--32');
const env = { ...process.env, WINEPREFIX: process.env.UNDAE_WINEPREFIX || path.join(os.homedir(), '.local/share/undae-sonicae/wine'), WINEDEBUG: '-all', WINEDLLOVERRIDES: 'mscoree,mshtml=' };
const host = spawn('wine', [exe, '--child'], { stdio: ['pipe', 'pipe', 'inherit'], env });
const t0 = performance.now();
const { port, token } = await new Promise((res, rej) => { let b = ''; host.stdout.on('data', d => { b += d; const i = b.indexOf('\n'); if (i >= 0) res(JSON.parse(b.slice(0, i))); }); host.once('exit', c => rej(new Error('host exited ' + c))); });
let failed = 0; const check = (name, ok, extra = '') => { console.log((ok ? 'ok   ' : 'FAIL ') + name + (extra ? '  ' + extra : '')); if (!ok) failed++; };
check('host starts under Wine', port > 0, Math.round(performance.now() - t0) + ' ms');

function connect(tok = token) {
  return new Promise((res, rej) => {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/?t=${tok}`); ws.binaryType = 'arraybuffer';
    const waitJson = new Map(), waitAudio = [], events = []; let seq = 0;
    ws.onmessage = e => { if (typeof e.data === 'string') { const m = JSON.parse(e.data); if (m.ev) events.push(m.ev + (m.open === false ? ':closed' : '')); else { waitJson.get(m.seq)?.(m); waitJson.delete(m.seq); } } else waitAudio.shift()?.(new Float32Array(e.data, 12)); };
    ws.onerror = () => rej(new Error('refused'));
    ws.onopen = () => res({
      events, close: () => ws.close(), tell: msg => ws.send(JSON.stringify(msg)),
      ask: msg => new Promise(r => { const s = ++seq; waitJson.set(s, r); ws.send(JSON.stringify({ ...msg, seq: s })); }),
      audio: (fill, n = 128) => new Promise(r => { const b = new ArrayBuffer(12 + n * 8); new Uint32Array(b, 0, 3).set([1, 1, n]); const f = new Float32Array(b, 12); for (let i = 0; i < f.length; i++) f[i] = typeof fill === 'function' ? fill(i >> 1) : fill; waitAudio.push(r); ws.send(b); }),
    });
  });
}
const peak = a => a.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
let phase = 0; const sine = () => { const base = phase; phase += 128; return i => 0.5 * Math.sin((base + i) * 2 * Math.PI * 440 / 48000); };

try {
  check('wrong token is refused', await connect('nope').then(() => false, () => true));
  const c = await connect();
  const hello = await c.ask({ op: 'hello' }); check('hello', hello.ok && hello.formats.includes('vst2'), JSON.stringify(hello));
  check('audio passes through with no plug-in', Math.abs(peak(await c.audio(0.5)) - 0.5) < 1e-4);
  const scan = await c.ask({ op: 'scan', paths: folders }); check('scan', scan.ok, scan.plugins.map(p => `${p.name}${p.instrument ? ' (instrument)' : ''} by ${p.vendor || '?'}`).join('; ') || 'nothing found');
  for (const pl of scan.plugins) {
    const d = await connect(), t = performance.now(), load = await d.ask({ op: 'load', path: pl.path, sampleRate: 48000 });
    check(`load ${pl.name}`, load.ok, load.ok ? `${Math.round(performance.now() - t)} ms, ${load.params.length} parameters, in ${load.inputs} out ${load.outputs}, notes ${load.notes}, has editor ${load.editor}` : load.error);
    if (!load.ok) { d.close(); continue; }
    console.log('       first parameters: ' + load.params.slice(0, 6).map(p => `${p.name}=${p.text}`).join(', '));
    if (pl.instrument) {
      let quiet = 0; for (let i = 0; i < 20; i++) quiet = Math.max(quiet, peak(await d.audio(0)));
      d.tell({ op: 'note', on: true, key: 60, vel: 0.9 }); let loud = 0; for (let i = 0; i < 200; i++) loud = Math.max(loud, peak(await d.audio(0)));
      check(`${pl.name}: a note makes sound`, loud > 0.001 && loud > quiet * 4, `before ${quiet.toFixed(4)}, during ${loud.toFixed(4)}`);
      d.tell({ op: 'note', on: false, key: 60 });
    } else {
      phase = 0; let wet = 0, diff = 0; for (let i = 0; i < 100; i++) { const f = sine(), base = phase - 128, o = await d.audio(f); wet = Math.max(wet, peak(o)); for (let k = 0; k < 128; k++) diff = Math.max(diff, Math.abs(o[k * 2] - f(k))); void base; }
      check(`${pl.name}: processes audio`, wet > 0.01, `output peak ${wet.toFixed(3)}, largest change to the signal ${diff.toFixed(3)}`);
    }
    if (load.params.length) {
      const p0 = load.params.find(p => !p.readonly && !p.hidden) || load.params[0]; d.tell({ op: 'param', id: p0.id, value: p0.value > 0.5 ? 0.1 : 0.9 }); await d.audio(0);
      const now = await d.ask({ op: 'params' }); check(`${pl.name}: parameter change sticks`, now.ok && Math.abs(now.params.find(p => p.id === p0.id).value - (p0.value > 0.5 ? 0.1 : 0.9)) < 0.01, `${p0.name}: ${p0.text} -> ${now.params.find(p => p.id === p0.id).text}`);
      const st = await d.ask({ op: 'state' }); d.tell({ op: 'param', id: p0.id, value: 0.5 }); await d.audio(0);
      const back = await d.ask({ op: 'state', data: st.data }); check(`${pl.name}: state saves and restores`, st.ok && back.ok && Math.abs(back.params.find(p => p.id === p0.id).value - now.params.find(p => p.id === p0.id).value) < 0.01, `${st.data.length} characters`);
    }
    if (load.gui && process.env.DISPLAY) {                                 // the plug-in's own window
      const onScreen = () => { const l = execFileSync('xwininfo', ['-root', '-tree'], { encoding: 'utf8' }).split('\n').find(x => x.includes('"' + load.name + '"') && !x.includes('mutter-x11-frames')); return l ? { id: l.match(/0x[0-9a-f]+/)[0], size: (l.match(/\d+x\d+/) || [''])[0] } : null; };
      const pause = ms => new Promise(r => setTimeout(r, ms));
      const g = await d.ask({ op: 'gui', show: true }); await pause(800); const w = onScreen();
      check(`${pl.name}: window opens`, g.ok && g.open && !!w, w ? w.size : g.error || 'not on screen');
      if (w) { execFileSync(path.join(here, 'bin', 'xpoke'), ['close', w.id]); await pause(800); check(`${pl.name}: closing the window is reported`, d.events.includes('gui:closed') && !onScreen(), d.events.join(',')); }
    }
    const t1 = performance.now(); for (let i = 0; i < 1000; i++) await d.audio(0.1); const per = (performance.now() - t1) / 1000;
    check(`${pl.name}: round trip per 128-frame block`, per < 2.6, per.toFixed(3) + ' ms (a block lasts 2.67 ms at 48 kHz)');
    d.close();
  }
  c.close(); await new Promise(r => setTimeout(r, 300));
  check('host survives disconnects', host.exitCode === null);
} catch (e) { check('test run', false, String(e && e.stack || e)); }
host.stdin.end(); await new Promise(r => setTimeout(r, 1500));
check('host exits when the app goes away', host.exitCode !== null); host.kill();
console.log(failed ? `${failed} FAILED` : 'all passed');
process.exit(failed ? 1 : 0);
