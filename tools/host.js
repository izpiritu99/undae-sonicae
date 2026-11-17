// Starts the plug-in hosts and reports where they listen.
//   startHost(bin, bundled)  the native CLAP host (native/host.c)       -> { port, token, bundled, process } or null
//   startWineHost(exe)       a Windows VST host under Wine (native/win)  -> { port, token, process } or null
//                            (undae-vsthost.exe for 64-bit plug-ins, undae-vsthost32.exe for 32-bit ones)
// Both resolve with null when the program is missing or will not start. `bundled` is a folder of plug-ins shipped with
// the app, scanned along with the usual places.
const { spawn, spawnSync } = require('child_process'), fs = require('fs'), os = require('os'), path = require('path'), crypto = require('crypto');

function firstLine(p, extra) {
  return new Promise(resolve => {
    let buf = '';
    p.stdout.on('data', d => {
      buf += d; const i = buf.indexOf('\n'); if (i < 0) return;
      try { const j = JSON.parse(buf.slice(0, i)); resolve(j.port ? { port: j.port, token: j.token, ...extra, process: p } : null); } catch { resolve(null); }
    });
    p.on('error', () => resolve(null));
    p.on('exit', () => resolve(null));
  });
}

function startHost(bin, bundled, serial) {
  if (!fs.existsSync(bin)) return Promise.resolve(null);
  try { return firstLine(spawn(bin, ['--child'], { stdio: ['ignore', 'pipe', 'inherit'], env: { ...process.env, UNDAE_SERIAL: serial ? '1' : '0' } }), { bundled }); } catch { return Promise.resolve(null); }
}

// Wine keeps its own Windows environment (a "prefix") for the app, apart from any the user has; creating it the first
// time takes several seconds. The host exits when its stdin closes, which happens when this process ends.
const winePrefix = () => process.env.UNDAE_WINEPREFIX || path.join(os.homedir(), '.local', 'share', 'undae-sonicae', 'wine');
const vstFolder = () => path.join(os.homedir(), '.vst-win');
function startWineHost(exe, serial) {
  if (!fs.existsSync(exe) || spawnSync('which', ['wine']).status !== 0) return Promise.resolve(null);
  try {
    fs.mkdirSync(path.dirname(winePrefix()), { recursive: true });
    const env = { ...process.env, WINEPREFIX: winePrefix(), WINEDEBUG: '-all', WINEDLLOVERRIDES: 'mscoree,mshtml=', UNDAE_HOST_TOKEN: crypto.randomBytes(16).toString('hex'), UNDAE_SERIAL: serial ? '1' : '0' };
    return firstLine(spawn('wine', [exe, '--child'], { stdio: ['pipe', 'pipe', 'ignore'], env }), { folder: vstFolder() });
  } catch { return Promise.resolve(null); }
}

// What /host.json answers: { port, token, bundled, win: { port, token, folder } | null, win32: same, winPending }.
function hostState() {
  const st = { clap: null, win: null, win32: null, winPending: false };
  st.start = async (bin, exe, serial = false) => {   // serial: multi-threaded processing switched off
    st.winPending = true; let left = 2; const done = () => { if (!--left) st.winPending = false; };   // slow the first time: do not wait for them
    startWineHost(exe, serial).then(w => { st.win = w; done(); });
    startWineHost(exe.replace(/\.exe$/, '32.exe'), serial).then(w => { st.win32 = w; done(); });
    st.clap = await startHost(path.join(bin, 'undae-host'), bin, serial);
  };
  const live = h => h && h.process.exitCode === null;
  st.json = () => ({ ...(live(st.clap) ? { port: st.clap.port, token: st.clap.token, bundled: st.clap.bundled } : {}), win: live(st.win) ? { port: st.win.port, token: st.win.token, folder: st.win.folder } : null, win32: live(st.win32) ? { port: st.win32.port, token: st.win32.token, folder: st.win32.folder } : null, winPending: st.winPending });
  st.stop = () => { for (const h of [st.clap, st.win, st.win32]) if (live(h)) h.process.kill(); };
  return st;
}
module.exports = { startHost, startWineHost, hostState };

// Settings the Electron main process needs before the window exists (/engine.json): { threads: true|false }.
function engineSettings(file) {
  const get = () => { try { return { threads: true, ...JSON.parse(fs.readFileSync(file, 'utf8')) }; } catch { return { threads: true }; } };
  const set = o => { try { fs.mkdirSync(path.dirname(file), { recursive: true }); fs.writeFileSync(file, JSON.stringify({ ...get(), ...o })); return true; } catch { return false; } };
  return { get, set };
}
module.exports.engineSettings = engineSettings;

// Load of the app's own processes (Electron app metrics) plus the plug-in hosts, read from /proc. cpu: percent of all
// cores; ram: MB of resident memory.
function loadStats(app, hosts) {
  const os = require('os'), cores = os.cpus().length, tick = 100, last = new Map();
  const proc = pid => { try { const st = fs.readFileSync(`/proc/${pid}/stat`, 'utf8').split(') ')[1].split(' '), rss = +fs.readFileSync(`/proc/${pid}/statm`, 'utf8').split(' ')[1] * 4096; return { t: (+st[11] + +st[12]) / tick, rss }; } catch { return null; } };
  return () => {
    const m = app.getAppMetrics(); let cpu = m.reduce((s, x) => s + (x.cpu ? x.cpu.percentCPUUsage : 0), 0), ram = m.reduce((s, x) => s + (x.memory ? x.memory.workingSetSize : 0), 0) / 1024;
    const now = Date.now() / 1000;
    for (const h of [hosts.clap, hosts.win, hosts.win32]) {
      if (!h || h.process.exitCode !== null) continue;
      const p = proc(h.process.pid); if (!p) continue;
      const prev = last.get(h.process.pid); last.set(h.process.pid, { t: p.t, at: now });
      if (prev && now > prev.at) cpu += (p.t - prev.t) / (now - prev.at) * 100;
      ram += p.rss / 1048576;
    }
    return { cpu: Math.min(100, cpu / cores), ram };
  };
}
module.exports.loadStats = loadStats;
