// Plug-in sharing over the local network: other devices (the Android app, another computer) use this computer's plug-in
// hosts. Off until switched on in Settings > Plug-ins. The hosts themselves keep listening on 127.0.0.1 only; this is
// a small gateway in front of them:
//
//   GET /share.json?code=123456          which hosts are running (no host tokens leave this computer)
//   WebSocket /clap|/win|/win32?code=…   piped straight through to that host, with the host's own token added
//
// Every request needs the pairing code shown in Settings (6 digits, new on request). Ten wrong codes from one address
// within a minute lock that address out for a minute. Traffic is plain HTTP / WebSocket: meant for a home network.
const http = require('http'), net = require('net'), fs = require('fs'), os = require('os'), crypto = require('crypto');

const DEFAULT_PORT = 47800;
const newCode = () => String(crypto.randomInt(0, 1e6)).padStart(6, '0');

function shareServer(hosts, file) {
  let cfg = { on: false, port: DEFAULT_PORT, code: newCode() };
  try { cfg = { ...cfg, ...JSON.parse(fs.readFileSync(file, 'utf8')) }; } catch { /* first run */ }
  const save = () => { try { fs.writeFileSync(file, JSON.stringify(cfg)); } catch { /* read-only */ } };
  let server = null, error = '', clients = 0;
  const fails = new Map();   // ip -> { n, since, until }
  const blocked = ip => { const f = fails.get(ip); return f && f.until > Date.now(); };
  const fail = ip => { const now = Date.now(), f = fails.get(ip) || { n: 0, since: now, until: 0 }; if (now - f.since > 60000) { f.n = 0; f.since = now; } if (++f.n >= 10) f.until = now + 60000; fails.set(ip, f); };
  const codeOk = (url, ip) => { const c = new URL(url, 'http://x').searchParams.get('code') || ''; const x = Buffer.from(c), y = Buffer.from(cfg.code), ok = x.length === y.length && crypto.timingSafeEqual(x, y); if (!ok) fail(ip); return ok; };
  const target = name => { const j = hosts.json(); return name === 'clap' ? (j.port ? j : null) : j[name] || null; };
  const CORS = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': '*', 'Cache-Control': 'no-store' };

  function onRequest(req, res) {
    const ip = req.socket.remoteAddress;
    if (req.method === 'OPTIONS') { res.writeHead(204, CORS); return res.end(); }
    if (blocked(ip)) { res.writeHead(429, CORS); return res.end(); }
    if (new URL(req.url, 'http://x').pathname !== '/share.json') { res.writeHead(404, CORS); return res.end(); }
    if (!codeOk(req.url, ip)) { res.writeHead(403, { ...CORS, 'Content-Type': 'application/json' }); return res.end(JSON.stringify({ ok: false, error: 'Wrong pairing code' })); }
    const j = hosts.json();
    res.writeHead(200, { ...CORS, 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, name: os.hostname(), clap: !!j.port, bundled: j.bundled || null, win: j.win ? { folder: j.win.folder } : null, win32: j.win32 ? { folder: j.win32.folder } : null, winPending: !!j.winPending }));
  }
  // WebSocket: check the code, then hand the very same handshake to the host with its token in place of ours. The Origin
  // header goes (the hosts accept only their own computer's pages; here the code is the check). After that, bytes flow.
  function onUpgrade(req, sock, head) {
    const ip = req.socket.remoteAddress, name = new URL(req.url, 'http://x').pathname.slice(1), h = target(name);
    if (blocked(ip) || !['clap', 'win', 'win32'].includes(name) || !codeOk(req.url, ip) || !h) { sock.end('HTTP/1.1 403 Forbidden\r\n\r\n'); return; }
    const up = net.connect(h.port, '127.0.0.1');
    sock.setNoDelay(true); up.setNoDelay(true);   // small audio blocks: send each at once
    up.on('connect', () => {
      let out = `GET /?t=${h.token} HTTP/1.1\r\n`;
      for (let i = 0; i < req.rawHeaders.length; i += 2) if (!/^(origin|host)$/i.test(req.rawHeaders[i])) out += `${req.rawHeaders[i]}: ${req.rawHeaders[i + 1]}\r\n`;
      up.write(out + 'Host: 127.0.0.1\r\n\r\n'); if (head && head.length) up.write(head);
      clients++; up.pipe(sock); sock.pipe(up);
    });
    let gone = false; const end = () => { if (gone) return; gone = true; clients = Math.max(0, clients - 1); up.destroy(); sock.destroy(); };
    up.on('error', end); sock.on('error', end); up.on('close', end); sock.on('close', end);
  }

  function start() {
    stop(); error = '';
    return new Promise(res => {
      const s = http.createServer(onRequest); s.on('upgrade', onUpgrade);
      s.on('error', e => { error = e.code === 'EADDRINUSE' ? `Port ${cfg.port} is already in use` : e.message; server = null; res(); });
      s.listen(cfg.port, '0.0.0.0', () => { server = s; res(); });
    });
  }
  function stop() { if (server) { server.close(); server = null; } }
  const addresses = () => Object.values(os.networkInterfaces()).flat().filter(a => a && a.family === 'IPv4' && !a.internal).map(a => a.address);
  const api = {
    get: () => ({ on: cfg.on, port: cfg.port, code: cfg.code, running: !!server, error, clients, addresses: addresses() }),
    async set(o) {
      if (typeof o.on === 'boolean') cfg.on = o.on;
      if (Number.isInteger(o.port) && o.port >= 1024 && o.port <= 65535) cfg.port = o.port;
      if (o.newCode) cfg.code = newCode();
      save(); if (cfg.on) await start(); else stop();
      return api.get();
    },
    start: () => (cfg.on ? start() : Promise.resolve()), stop,
  };
  return api;
}
module.exports = { shareServer, DEFAULT_PORT };
