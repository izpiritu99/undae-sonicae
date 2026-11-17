// String building, base64, SHA-1, minimal JSON reading and the WebSocket handshake, shared by the Linux plug-in host
// (host.c) and the Windows one that runs under Wine (win/vsthost.c). The includer provides the C library declarations.
#pragma once
typedef struct { char *p; size_t n, cap; } sb_t;
static void sb_put(sb_t *s, const void *d, size_t n) {
  if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2; s->p = realloc(s->p, s->cap); }
  memcpy(s->p + s->n, d, n); s->n += n; s->p[s->n] = 0;
}
static void sb_str(sb_t *s, const char *z) { sb_put(s, z, strlen(z)); }
static void sb_fmt(sb_t *s, const char *f, ...) { char b[512]; va_list a; va_start(a, f); int n = vsnprintf(b, sizeof b, f, a); va_end(a); if (n < 0 || n >= (int)sizeof b) n = (int)sizeof b - 1; b[n] = 0; sb_put(s, b, (size_t)n); }
static void sb_json(sb_t *s, const char *z) {                      // a JSON string literal
  sb_put(s, "\"", 1);
  for (const unsigned char *c = (const unsigned char *)(z ? z : ""); *c; c++) {
    if (*c == '"' || *c == '\\') { sb_put(s, "\\", 1); sb_put(s, c, 1); }
    else if (*c < 32) sb_fmt(s, "\\u%04x", *c);
    else sb_put(s, c, 1);
  }
  sb_put(s, "\"", 1);
}
static void sb_num(sb_t *s, double v) { if (!__builtin_isfinite(v)) v = 0; sb_fmt(s, "%.9g", v); }

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_enc(sb_t *s, const uint8_t *d, size_t n) {
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    char o[4] = { B64[v >> 18], B64[v >> 12 & 63], i + 1 < n ? B64[v >> 6 & 63] : '=', i + 2 < n ? B64[v & 63] : '=' };
    sb_put(s, o, 4);
  }
}
static size_t b64_dec(const char *z, size_t n, uint8_t *out) {
  uint32_t v = 0; int bits = 0; size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    const char *q = z[i] && z[i] != '=' ? strchr(B64, z[i]) : NULL; if (!q) continue;
    v = v << 6 | (uint32_t)(q - B64); bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = v >> bits & 255; }
  }
  return o;
}

static uint32_t rol(uint32_t v, int n) { return v << n | v >> (32 - n); }
static void sha1(const uint8_t *d, size_t n, uint8_t out[20]) {
  uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
  size_t total = ((n + 8) / 64 + 1) * 64; uint8_t *m = calloc(total, 1); memcpy(m, d, n); m[n] = 0x80;
  for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)((uint64_t)n * 8 >> (8 * i));
  for (size_t o = 0; o < total; o += 64) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], e4 = h[3], e = h[4];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)m[o + i * 4] << 24 | m[o + i * 4 + 1] << 16 | m[o + i * 4 + 2] << 8 | m[o + i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
      uint32_t f = i < 20 ? (b & c) | (~b & e4) : i < 40 ? b ^ c ^ e4 : i < 60 ? (b & c) | (b & e4) | (c & e4) : b ^ c ^ e4;
      uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i]; e = e4; e4 = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += e4; h[4] += e;
  }
  free(m);
  for (int i = 0; i < 5; i++) { out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i]; }
}

// Minimal JSON reading: values of the top-level object only, which is all the protocol uses.
static const char *jws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static const char *jskip(const char *p) {                           // p at a value; returns the first char after it
  if (*p == '"') { for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++; return *p ? p + 1 : p; }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    for (; *p; p++) {
      if (*p == '"') { p = jskip(p) - 1; if (!p[1]) return p + 1; }
      else if (*p == '{' || *p == '[') depth++;
      else if (*p == '}' || *p == ']') { if (!--depth) return p + 1; }
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']') p++;
  return p;
}
static const char *jget(const char *js, const char *key) {
  const char *p = jws(js); if (*p != '{') return NULL;
  size_t kl = strlen(key);
  for (p = jws(p + 1); *p == '"';) {
    const char *k = p + 1, *e = jskip(p); const char *v = jws(e); if (*v != ':') return NULL; v = jws(v + 1);
    if ((size_t)(e - 1 - k) == kl && !memcmp(k, key, kl)) return v;
    p = jws(jskip(v)); if (*p == ',') p = jws(p + 1);
  }
  return NULL;
}
static char *jstr_at(const char *v) {                                // decoded copy of a JSON string, or NULL
  if (!v || *v != '"') return NULL;
  const char *e = jskip(v); char *out = malloc((size_t)(e - v) + 1), *o = out;
  for (const char *p = v + 1; p < e - 1; p++) {
    if (*p != '\\') { *o++ = *p; continue; }
    p++;
    if (*p == 'n') *o++ = '\n'; else if (*p == 't') *o++ = '\t'; else if (*p == 'r') *o++ = '\r'; else if (*p == 'b') *o++ = '\b'; else if (*p == 'f') *o++ = '\f';
    else if (*p == 'u' && p + 4 < e) {
      unsigned cp = 0; sscanf(p + 1, "%4x", &cp); p += 4;
      if (cp < 0x80) *o++ = (char)cp; else if (cp < 0x800) { *o++ = (char)(0xC0 | cp >> 6); *o++ = (char)(0x80 | (cp & 63)); }
      else { *o++ = (char)(0xE0 | cp >> 12); *o++ = (char)(0x80 | (cp >> 6 & 63)); *o++ = (char)(0x80 | (cp & 63)); }
    } else *o++ = *p;
  }
  *o = 0; return out;
}
static char *jstr(const char *js, const char *key) { return jstr_at(jget(js, key)); }
static double jnum(const char *js, const char *key, double def) { const char *v = jget(js, key); return v && (*v == '-' || (*v >= '0' && *v <= '9')) ? strtod(v, NULL) : def; }
static int jtrue(const char *js, const char *key) { const char *v = jget(js, key); return v && !strncmp(v, "true", 4); }


// WebSocket opening handshake. `req` is the HTTP request; on success `out` holds the reply to send and 1 is returned.
// The request must carry ?t=<token> and, if it names an Origin, a loopback one.
static const char *find_header(const char *req, const char *name) {     // case-insensitive "\r\nName:" search; returns the value
  size_t n = strlen(name);
  for (const char *p = req; (p = strstr(p, "\r\n")); p += 2) {
    size_t i = 0; while (i < n && p[2 + i] && (p[2 + i] | 32) == (name[i] | 32)) i++;
    if (i == n && p[2 + n] == ':') { p += 3 + n; while (*p == ' ') p++; return p; }
  }
  return NULL;
}
static int ws_accept(const char *req, const char *token, sb_t *out) {
  size_t tl = strlen(token); const char *line_end = strstr(req, "\r\n"), *t = strstr(req, "t=");
  while (t && (strncmp(t + 2, token, tl) || (t[2 + tl] != ' ' && t[2 + tl] != '&'))) t = strstr(t + 1, "t=");
  int ok = !strncmp(req, "GET ", 4) && t && line_end && t < line_end;
  const char *origin = find_header(req, "Origin"), *key = find_header(req, "Sec-WebSocket-Key");
  if (origin && strncmp(origin, "http://127.0.0.1", 16) && strncmp(origin, "http://localhost", 16)) ok = 0;
  if (!ok || !key) return 0;
  char cat[200]; size_t kl = 0; while (key[kl] && key[kl] != '\r' && key[kl] != '\n' && key[kl] != ' ') kl++;
  if (kl > 100) return 0;
  memcpy(cat, key, kl); memcpy(cat + kl, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 37);
  uint8_t dig[20]; sha1((const uint8_t *)cat, kl + 36, dig);
  sb_str(out, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "); b64_enc(out, dig, 20); sb_str(out, "\r\n\r\n");
  return 1;
}
