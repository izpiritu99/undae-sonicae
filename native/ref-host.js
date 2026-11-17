#!/usr/bin/env node
// Reference plug-in host: speaks native/PROTOCOL.md but loads no plug-ins. It halves the level of whatever it is sent,
// so the audio bridge can be tested end to end before the real (C) host exists. No dependencies.
//
//   node native/ref-host.js        prints one JSON line: {"port":N,"token":"..."}
const http = require('http'), crypto = require('crypto');

const token = process.env.UNDAE_HOST_TOKEN || crypto.randomBytes(16).toString('hex');
const okOrigin = o => !o || /^http:\/\/(127\.0\.0\.1|localhost)(:\d+)?$/.test(o);

function frame(op, payload) {
  const n = payload.length, head = n < 126 ? Buffer.from([0x80 | op, n]) : n < 65536 ? Buffer.from([0x80 | op, 126, n >> 8, n & 255]) : (() => { const b = Buffer.alloc(10); b[0] = 0x80 | op; b[1] = 127; b.writeBigUInt64BE(BigInt(n), 2); return b; })();
  return Buffer.concat([head, payload]);
}

function onAudio(sock, buf) {
  const type = buf.readUInt32LE(0), frames = buf.readUInt32LE(8);
  if (type !== 1 || buf.length < 12 + frames * 8) return;
  const out = Buffer.from(buf);                                                         // same header: type, instance, frames
  for (let k = 0, p = 12; k < frames * 2; k++, p += 4) out.writeFloatLE(buf.readFloatLE(p) * 0.5, p);
  sock.write(frame(2, out));
}

function onText(sock, text) {
  let m; try { m = JSON.parse(text); } catch { return; }
  const reply = o => sock.write(frame(1, Buffer.from(JSON.stringify({ re: m.op, seq: m.seq, ...o }))));
  if (m.op === 'hello') reply({ ok: true, host: 'undae-ref-host', version: 1, formats: [] });
  else if (m.op === 'scan') reply({ ok: true, plugins: [] });
  else reply({ ok: false, error: 'The reference host cannot load plug-ins' });
}

const server = http.createServer((req, res) => { res.writeHead(404); res.end(); });
server.on('upgrade', (req, sock) => {
  const url = new URL(req.url, 'http://x');
  if (url.searchParams.get('t') !== token || !okOrigin(req.headers.origin)) { sock.end('HTTP/1.1 403 Forbidden\r\n\r\n'); return; }
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  sock.setNoDelay(true);
  let pend = Buffer.alloc(0);
  sock.on('data', chunk => {
    pend = pend.length ? Buffer.concat([pend, chunk]) : chunk;
    for (;;) {
      if (pend.length < 2) return;
      const op = pend[0] & 15, masked = pend[1] & 128; let n = pend[1] & 127, at = 2;
      if (n === 126) { if (pend.length < 4) return; n = pend.readUInt16BE(2); at = 4; }
      else if (n === 127) { if (pend.length < 10) return; n = Number(pend.readBigUInt64BE(2)); at = 10; }
      const need = at + (masked ? 4 : 0) + n; if (pend.length < need) return;
      const body = Buffer.from(pend.subarray(need - n, need));
      if (masked) { const k = pend.subarray(at, at + 4); for (let i = 0; i < n; i++) body[i] ^= k[i & 3]; }
      pend = pend.subarray(need);
      if (op === 2) onAudio(sock, body); else if (op === 1) onText(sock, body.toString()); else if (op === 9) sock.write(frame(10, body)); else if (op === 8) { sock.end(frame(8, Buffer.alloc(0))); return; }
    }
  });
  sock.on('error', () => {});
});
server.listen(+process.env.UNDAE_HOST_PORT || 0, '127.0.0.1', () => console.log(JSON.stringify({ port: server.address().port, token })));
