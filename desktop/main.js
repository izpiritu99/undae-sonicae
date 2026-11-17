// Electron shell for Linux (also runs on Windows/macOS). Serves ../app from a loopback port and opens it in one window.
const { app, BrowserWindow, session, shell } = require('electron');
const path = require('path');
const { serve } = require('../tools/serve.js');
const { hostState, engineSettings, loadStats } = require('../tools/host.js');
const { shareServer } = require('../tools/share.js');
const engine = engineSettings(path.join(app.getPath('userData'), 'engine.json'));

let server; const hosts = hostState(), share = shareServer(hosts, path.join(app.getPath('userData'), 'share.json'));
async function create() {
  if (!server) {
    // The plug-in hosts and the test plug-in sit next to app.asar when packaged, in native/bin/ during development.
    const bin = app.isPackaged ? process.resourcesPath : path.join(__dirname, '..', 'native', 'bin');
    await hosts.start(bin, path.join(bin, 'undae-vsthost.exe'), !engine.get().threads);
    server = await serve(path.join(__dirname, '..', 'app'), 0, '127.0.0.1', hosts.json, engine, loadStats(app, hosts), share);
    await share.start();   // plug-in sharing on the network, when switched on in Settings > Plug-ins
  }
  const origin = 'http://127.0.0.1:' + server.address().port;
  // The app only ever asks for the microphone and MIDI; grant those to our own origin and nothing else.
  const ok = (url, perm) => String(url || '').startsWith(origin) && ['media', 'midi', 'midiSysex'].includes(perm);
  session.defaultSession.setPermissionRequestHandler((wc, perm, cb) => cb(ok(wc.getURL(), perm)));
  session.defaultSession.setPermissionCheckHandler((wc, perm, reqOrigin) => ok(reqOrigin || (wc && wc.getURL()), perm));
  const win = new BrowserWindow({
    width: 1360, height: 860, minWidth: 380, minHeight: 520, backgroundColor: '#16181c', autoHideMenuBar: true, title: 'Undae Sonicae',
    icon: path.join(__dirname, '..', 'app', 'icons', 'icon-512.png'),
    webPreferences: { contextIsolation: true, nodeIntegration: false, sandbox: true, backgroundThrottling: false },
  });
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
  win.loadURL(origin + '/index.html');
}
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
// Multi-threaded processing (Settings > Audio > CPU): the audio worklet thread runs at real-time priority, plug-ins run in
// parallel, and timers in the window are never slowed down. Off: plug-ins take turns on one thread.
if (engine.get().threads) { app.commandLine.appendSwitch('enable-features', 'AudioWorkletRealtimeThread'); app.commandLine.appendSwitch('disable-background-timer-throttling'); app.commandLine.appendSwitch('disable-renderer-backgrounding'); }
app.whenReady().then(create);
app.on('activate', () => { if (!BrowserWindow.getAllWindows().length) create(); });
app.on('quit', () => { share.stop(); hosts.stop(); });
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
