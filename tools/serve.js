// Tiny static server. `npm start` serves the app for a browser; the Electron shell reuses serve() on a random port
// so the app always runs from http://127.0.0.1 (ES modules, microphone and Web MIDI all need a real origin).
const http = require('http'), fs = require('fs'), path = require('path');
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.webmanifest': 'application/manifest+json', '.svg': 'image/svg+xml', '.png': 'image/png', '.wav': 'audio/wav' };

// `plug` returns what the app reads from /host.json: where the plug-in hosts listen (see tools/host.js), or null.
// `engine` ({ get, set }) backs /engine.json: settings that apply when the app next starts.
// `stats` returns { cpu, ram } for /stats.json (the top bar's load display), or null.
// `share` (tools/share.js) backs /share-config.json: sharing this computer's plug-ins on the network (desktop app only).
function serve(root, port = 0, host = '127.0.0.1', plug = () => null, engine = null, stats = null, share = null) {
  root = path.resolve(root);
  return new Promise((resolve, reject) => {
    const s = http.createServer((req, res) => {
      let p = decodeURIComponent(req.url.split('?')[0]);
      if (p === '/stats.json') { res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' }); return res.end(JSON.stringify(stats ? stats() : null)); }
      if (p === '/engine.json' && engine) {
        const reply = o => { res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' }); res.end(JSON.stringify(o)); };
        if (req.method !== 'PUT') return reply(engine.get());
        let body = ''; req.on('data', d => { body += d; if (body.length > 10000) req.destroy(); });
        return req.on('end', () => { try { const o = JSON.parse(body); reply({ ok: engine.set({ threads: o.threads !== false }) }); } catch { reply({ ok: false }); } });
      }
      if (p === '/share-config.json' && share) {
        const reply = o => { res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' }); res.end(JSON.stringify(o)); };
        if (req.method !== 'PUT') return reply(share.get());
        let body = ''; req.on('data', d => { body += d; if (body.length > 10000) req.destroy(); });
        return req.on('end', async () => { try { reply(await share.set(JSON.parse(body))); } catch { reply(share.get()); } });
      }
      if (p === '/host.json') { res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'Cross-Origin-Resource-Policy': 'same-origin' }); return res.end(JSON.stringify(plug())); }
      if (p.endsWith('/')) p += 'index.html';
      const file = path.join(root, path.normalize(p));
      if (!file.startsWith(root)) { res.writeHead(403); return res.end(); }
      fs.readFile(file, (err, data) => {
        if (err) { res.writeHead(404); return res.end('Not found'); }
        // COOP + COEP make the page cross-origin isolated, which SharedArrayBuffer (the plug-in audio bridge) requires.
        res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-cache', 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Embedder-Policy': 'require-corp', 'Cross-Origin-Resource-Policy': 'same-origin' });
        res.end(data);
      });
    });
    s.on('error', reject);
    s.listen(port, host, () => resolve(s));
  });
}
module.exports = { serve };

if (require.main === module) {
  const bin = path.join(__dirname, '..', 'native', 'bin'), hosts = require('./host.js').hostState();
  hosts.start(bin, path.join(bin, 'undae-vsthost.exe'));
  serve(path.join(__dirname, '..', 'app'), +process.env.PORT || 5173, '127.0.0.1', hosts.json).then(s => console.log('Undae Sonicae running at http://localhost:' + s.address().port));
}
