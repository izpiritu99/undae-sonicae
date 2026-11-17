// Undae Sonicae Windows plug-in host. Runs under Wine and loads Windows VST2 plug-ins (.dll), speaking the same
// WebSocket protocol as the Linux host (native/PROTOCOL.md), so the app talks to it exactly as it talks to host.c.
// Built by native/build.sh with the ordinary Linux gcc and ld; see winshim.h for how.
//
// The VST2 binary interface below is written from its public description (struct layout and opcode numbers); no
// Steinberg SDK code is used.
#include "winshim.h"
#include "../util.h"

#define MAXF 4096
#define BLOCK 1024                // largest block handed to a plug-in in one call
#define MAXMSG (64u << 20)

// ---------------------------------------------------------------------------------------------- VST2 interface
typedef struct AEffect AEffect;
typedef intptr_t (*master_fn)(AEffect *, int32_t op, int32_t index, intptr_t value, void *ptr, float opt);
struct AEffect {
  int32_t magic;                                                          // 'VstP'
  intptr_t (*dispatcher)(AEffect *, int32_t op, int32_t index, intptr_t value, void *ptr, float opt);
  void (*process)(AEffect *, float **in, float **out, int32_t frames);    // old style: adds to the output
  void (*setParameter)(AEffect *, int32_t index, float value);
  float (*getParameter)(AEffect *, int32_t index);
  int32_t numPrograms, numParams, numInputs, numOutputs, flags;
  intptr_t resvd1, resvd2;
  int32_t initialDelay, realQualities, offQualities; float ioRatio;
  void *object, *user;
  int32_t uniqueID, version;
  void (*processReplacing)(AEffect *, float **in, float **out, int32_t frames);
  void (*processDoubleReplacing)(AEffect *, double **in, double **out, int32_t frames);
  char future[56];
};
enum { effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
       effEditGetRect = 13, effEditOpen = 14, effEditClose = 15, effEditIdle = 19, effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48, effCanDo = 51, effStartProcess = 71, effStopProcess = 72 };
enum { flagHasEditor = 1, flagCanReplacing = 1 << 4, flagProgramChunks = 1 << 5, flagIsSynth = 1 << 8 };
enum { amAutomate = 0, amVersion = 1, amGetTime = 7, amSizeWindow = 15, amGetSampleRate = 16, amGetBlockSize = 17, amGetProcessLevel = 23, amGetVendorString = 32, amGetProductString = 33, amGetVendorVersion = 34, amCanDo = 37, amGetLanguage = 38, amBeginEdit = 43, amEndEdit = 44 };
typedef struct { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos; int32_t sigNum, sigDen, smpteOffset, smpteRate, toNextClock, flags; } VstTimeInfo;
typedef struct { int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset; char midi[4]; char detune, offVelocity, r1, r2; } VstMidiEvent;
#define MAXQ 256
typedef struct { int32_t numEvents; intptr_t reserved; VstMidiEvent *events[MAXQ]; } VstEvents;

// ---------------------------------------------------------------------------------------------- connections
typedef struct conn {
  SOCKET fd; CRITICAL_SECTION wlock, qlock; atomic_int dead; struct conn *next; HANDLE thread;
  char *rbuf; size_t rpos, rlen; char *wbuf;                              // socket calls are slow under Wine: read in bulk, send each message in one call
  char *load_js;                                                          // the request that loaded the plug-in, to load it again
  void *lib; AEffect *fx; void *v3; char *name; double sr; int started; void *hwnd; atomic_int want_dirty, want_closed, want_rescan;   // fx: a VST2 plug-in, v3: a VST3 one
  atomic_int active, busy, want_out;
  float **in, **out; int nin, nout; VstTimeInfo time;
  VstMidiEvent midi[MAXQ], cur[MAXQ]; uint32_t mat[MAXQ]; uint8_t mtimed[MAXQ]; int nmidi; VstEvents events;
  struct { uint32_t at; int32_t id; float v; uint8_t timed; } pq[MAXQ], pcur[MAXQ]; int npq;                      // timed parameter changes (export automation)   // queued notes wait for their frame
  struct { int32_t id; float value; } out_ev[64]; int nout_ev;
} conn_t;
typedef struct job { conn_t *c; char *text; struct job *next; } job_t;

static CRITICAL_SECTION mlock; static HANDLE wake_ev; static job_t *jobs, *jobs_tail; static conn_t *conns, *loading; static char token[80];
static uint32_t main_tid; static int serial; static CRITICAL_SECTION serial_lock;
// Plug-ins often share tables between their instances and set them up when one is opened or closed. Audio threads hold
// this lock shared while a plug-in processes; opening and closing take it exclusively, so no instance is processing then.
static SRWLOCK load_lock;
static void wake(void) { SetEvent(wake_ev); }
static uint32_t frame_of(const char *p) {                                 // an audio frame number, modulo 2^32 (no floating point: see winshim.h)
  uint32_t v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (uint32_t)(*p++ - '0'); return v;
}
// 64 or 32 for a Windows DLL (the machine field of its PE header), 0 if it is not one. A host only loads its own kind.
static int pe_bits(const char *dos_path) {
  void *f = fopen(dos_path, "rb"); if (!f) return 0;
  uint8_t h[64], pe[6]; int bits = 0;
  if (fread(h, 1, 64, f) == 64 && h[0] == 'M' && h[1] == 'Z') {
    int32_t at = (int32_t)(h[60] | h[61] << 8 | h[62] << 16 | (uint32_t)h[63] << 24);
    if (!fseek(f, at, 0) && fread(pe, 1, 6, f) == 6 && !memcmp(pe, "PE\0\0", 4)) { int m = pe[4] | pe[5] << 8; bits = m == 0x8664 ? 64 : m == 0x14c ? 32 : 0; }
  }
  fclose(f); return bits;
}
#define HOST_BITS ((int)sizeof(void *) * 8)

static int write_all(SOCKET fd, const void *d, size_t n) { const char *p = d; while (n) { int k = send(fd, p, (int)(n > 65536 ? 65536 : n), 0); if (k <= 0) return 0; p += k; n -= (size_t)k; } return 1; }
#define RBUF 65536
static int read_all(conn_t *c, void *d, size_t n) {
  char *p = d;
  while (n) {
    if (c->rpos == c->rlen) { int k = recv(c->fd, c->rbuf, RBUF, 0); if (k <= 0) return 0; c->rpos = 0; c->rlen = (size_t)k; }
    size_t k = c->rlen - c->rpos < n ? c->rlen - c->rpos : n; memcpy(p, c->rbuf + c->rpos, k); c->rpos += k; p += k; n -= k;
  }
  return 1;
}
static void ws_send(conn_t *c, int op, const void *d, size_t n) {
  uint8_t h[10]; size_t hn = 2; h[0] = (uint8_t)(0x80 | op);
  if (n < 126) h[1] = (uint8_t)n; else if (n < 65536) { h[1] = 126; h[2] = (uint8_t)(n >> 8); h[3] = (uint8_t)n; hn = 4; }
  else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (8 * (7 - i))); hn = 10; }
  EnterCriticalSection(&c->wlock);
  if (n <= RBUF - 16) { memcpy(c->wbuf, h, hn); memcpy(c->wbuf + hn, d, n); write_all(c->fd, c->wbuf, hn + n); }
  else if (write_all(c->fd, h, hn)) write_all(c->fd, d, n);
  LeaveCriticalSection(&c->wlock);
}
static sb_t *capture;                                                     // set: replies are collected here instead of sent (see panic)
static void send_json(conn_t *c, sb_t *s) { if (capture) { sb_put(capture, s->p, s->n); free(s->p); return; } ws_send(c, 1, s->p, s->n); free(s->p); }
static sb_t reply_begin(const char *js, int ok) {
  sb_t s = { 0 }; char *op = jstr(js, "op"); sb_str(&s, "{\"re\":"); sb_json(&s, op); free(op);
  const char *seq = jget(js, "seq"); if (seq) { sb_str(&s, ",\"seq\":"); sb_put(&s, seq, (size_t)(jskip(seq) - seq)); }
  sb_str(&s, ok ? ",\"ok\":true" : ",\"ok\":false"); return s;
}
static void reply_err(conn_t *c, const char *js, const char *msg) { sb_t s = reply_begin(js, 0); sb_str(&s, ",\"error\":"); sb_json(&s, msg); sb_str(&s, "}"); send_json(c, &s); }

// ---------------------------------------------------------------------------------------------- plug-ins
#define WSTYLE 0x00CA0000u                                               /* caption, system menu, minimise box; not resizable */
static void size_window(conn_t *c, int32_t w, int32_t h) {                // make the client area w by h
  RECT r = { 0, 0, w, h }; AdjustWindowRectEx(&r, WSTYLE, 0, 0); SetWindowPos(c->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, 0x2 | 0x4 | 0x10);   // no move, no z-order, no activate
}
static void copy_str(void *dst, const char *src) { memcpy(dst, src, strlen(src) + 1); }
static intptr_t master(AEffect *e, int32_t op, int32_t index, intptr_t value, void *ptr, float opt) {   // the plug-in calling its host
  conn_t *c = e && e->user ? e->user : loading;
  switch (op) {
    case amAutomate:                                                      // a parameter moved, usually in the plug-in's window
      if (c) { EnterCriticalSection(&c->qlock); if (c->nout_ev < 64) { c->out_ev[c->nout_ev].id = index; c->out_ev[c->nout_ev].value = opt; c->nout_ev++; } LeaveCriticalSection(&c->qlock); atomic_store(&c->want_out, 1); atomic_store(&c->want_dirty, 1); wake(); }
      return 0;
    case amVersion: return 2400;
    case amSizeWindow: if (c && c->hwnd) { size_window(c, index, (int32_t)value); return 1; } return 0;
    case amGetTime: return c ? (intptr_t)&c->time : 0;
    case amGetSampleRate: return c ? (intptr_t)c->sr : 48000;
    case amGetBlockSize: return BLOCK;
    case amGetProcessLevel: return GetCurrentThreadId() == main_tid ? 1 : 2;
    case amGetVendorString: case amGetProductString: copy_str(ptr, "Undae Sonicae"); return 1;
    case amGetVendorVersion: return 100;
    case amCanDo: return ptr && (!strcmp(ptr, "sendVstEvents") || !strcmp(ptr, "sendVstMidiEvent") || !strcmp(ptr, "sendVstTimeInfo") || !strcmp(ptr, "supplyIdle")) ? 1 : 0;
    case amGetLanguage: return 1;
    case amBeginEdit: case amEndEdit: return 1;
  }
  return 0;
}

static char *dos_path(const char *unix_path) {                            // Wine maps drive Z: to the Linux root
  size_t n = strlen(unix_path); char *p = malloc(n + 3); p[0] = 'Z'; p[1] = ':';
  for (size_t i = 0; i <= n; i++) p[2 + i] = unix_path[i] == '/' ? '\\' : unix_path[i];
  return p;
}
static AEffect *open_plugin(const char *unix_path, void **lib, conn_t *c) {   // [main thread] load the .dll and open the effect
  char *p = dos_path(unix_path); void *h = LoadLibraryA(p); free(p); if (!h) return NULL;
  AEffect *(*entry)(master_fn) = GetProcAddress(h, "VSTPluginMain"); if (!entry) entry = GetProcAddress(h, "main");
  AcquireSRWLockExclusive(&load_lock);
  loading = c; AEffect *e = entry ? entry(master) : NULL;
  if (!e || e->magic != 0x56737450) { loading = NULL; ReleaseSRWLockExclusive(&load_lock); FreeLibrary(h); return NULL; }
  e->user = c; e->dispatcher(e, effOpen, 0, 0, NULL, 0); loading = NULL; ReleaseSRWLockExclusive(&load_lock); *lib = h; return e;
}
static void get_str(AEffect *e, int32_t op, int32_t index, char *out) { char b[512]; memset(b, 0, sizeof b); e->dispatcher(e, op, index, 0, b, 0); b[255] = 0; copy_str(out, b); }
static void trim(char *s) { size_t n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0; }

static void free_bufs(float **b, int n) { if (!b) return; for (int i = 0; i < n; i++) free(b[i]); free(b); }
static float **make_bufs(int n) { float **b = calloc((size_t)(n > 0 ? n : 1), sizeof *b); for (int i = 0; i < n; i++) b[i] = calloc(MAXF, sizeof(float)); return b; }

// --- the plug-in's own window ---------------------------------------------------------------------------------
typedef struct { int16_t top, left, bottom, right; } ERect;
static intptr_t WINAPI wndproc(void *w, uint32_t msg, uintptr_t wp, intptr_t lp);
static int make_window(conn_t *c) {                                       // [main thread] an empty top-level window for the plug-in to fill
  static int registered;
  if (!registered) { WNDCLASSEXA wc; memset(&wc, 0, sizeof wc); wc.size = sizeof wc; wc.proc = wndproc; wc.instance = GetModuleHandleA(NULL); wc.cursor = LoadCursorA(NULL, (const char *)32512); wc.name = "UndaePlugin"; RegisterClassExA(&wc); registered = 1; }
  c->hwnd = CreateWindowExA(0, "UndaePlugin", c->name ? c->name : "Plug-in", WSTYLE, (int32_t)0x80000000, (int32_t)0x80000000, 640, 480, NULL, NULL, GetModuleHandleA(NULL), NULL);
  if (c->hwnd) SetWindowLongPtrA(c->hwnd, -21, (intptr_t)c);
  return c->hwnd != NULL;
}
static void drop_window(conn_t *c) { void *w = c->hwnd; c->hwnd = NULL; SetWindowLongPtrA(w, -21, 0); DestroyWindow(w); }
#include "vst3win.h"
static void gui_close(conn_t *c) {                                        // [main thread]
  if (!c->hwnd) return;
  if (c->v3) v3_gui_close(c); else { KillTimer(c->hwnd, 1); c->fx->dispatcher(c->fx, effEditClose, 0, 0, NULL, 0); }
  drop_window(c);
  atomic_store(&c->want_dirty, 1);                                        // whatever was changed in the window should be saved
}
static intptr_t WINAPI wndproc(void *w, uint32_t msg, uintptr_t wp, intptr_t lp) {
  conn_t *c = (conn_t *)GetWindowLongPtrA(w, -21);                        // GWLP_USERDATA
  if (c && msg == 0x0113 && c->fx) { c->fx->dispatcher(c->fx, effEditIdle, 0, 0, NULL, 0); return 0; }   // WM_TIMER
  if (c && msg == 0x0010) { gui_close(c); atomic_store(&c->want_closed, 1); return 0; }                 // WM_CLOSE
  return DefWindowProcA(w, msg, wp, lp);
}
static const char *gui_show(conn_t *c) {                                  // [main thread] NULL on success, else why not
  if (c->v3) return v3_gui_show(c);
  AEffect *e = c->fx;
  if (!e || !(e->flags & flagHasEditor)) return "This plug-in has no window of its own";
  if (c->hwnd) { ShowWindow(c->hwnd, 5); return NULL; }
  if (!make_window(c)) return "Wine could not create a window";
  if (!e->dispatcher(e, effEditOpen, 0, 0, c->hwnd, 0)) { drop_window(c); return "The plug-in could not open its window"; }
  ERect *r = NULL; e->dispatcher(e, effEditGetRect, 0, 0, &r, 0);
  if (r && r->right > r->left && r->bottom > r->top) size_window(c, r->right - r->left, r->bottom - r->top);
  ShowWindow(c->hwnd, 5); SetTimer(c->hwnd, 1, 30, NULL);
  return NULL;
}

static void plug_unload(conn_t *c) {                                      // [main thread]
  if (c->v3) { gui_close(c); v3_unload(c); return; }
  if (!c->fx) return;
  gui_close(c);
  atomic_store(&c->active, 0); while (atomic_load(&c->busy)) Sleep(1);
  AEffect *e = c->fx; c->fx = NULL;
  if (c->started) { e->dispatcher(e, effStopProcess, 0, 0, NULL, 0); e->dispatcher(e, effMainsChanged, 0, 0, NULL, 0); }
  AcquireSRWLockExclusive(&load_lock); e->dispatcher(e, effClose, 0, 0, NULL, 0); ReleaseSRWLockExclusive(&load_lock); FreeLibrary(c->lib); c->lib = NULL; c->started = 0;
  free_bufs(c->in, c->nin); free_bufs(c->out, c->nout); c->in = c->out = NULL; c->nin = c->nout = 0; free(c->name); c->name = NULL;
}

static void put_params(conn_t *c, sb_t *s, int full) {                    // [main thread] ,"params":[...]
  if (c->v3) { v3_put_params(c, s, full); return; }
  AEffect *e = c->fx; sb_str(s, ",\"params\":[");
  for (int32_t i = 0; i < e->numParams; i++) {
    char disp[256], label[256], text[520]; get_str(e, effGetParamDisplay, i, disp); get_str(e, effGetParamLabel, i, label); trim(disp);
    _snprintf(text, sizeof text, "%s%s%s", disp, *label ? " " : "", label); text[sizeof text - 1] = 0;
    double v = e->getParameter(e, i);
    sb_fmt(s, "%s{\"id\":%d,\"value\":", i ? "," : "", i); sb_num(s, v); sb_str(s, ",\"text\":"); sb_json(s, text);
    if (full) { char name[256]; get_str(e, effGetParamName, i, name); trim(name); sb_str(s, ",\"name\":"); sb_json(s, name); sb_str(s, ",\"module\":\"\",\"min\":0,\"max\":1,\"def\":"); sb_num(s, v); sb_str(s, ",\"stepped\":false,\"hidden\":false,\"readonly\":false"); }
    sb_str(s, "}");
  }
  sb_str(s, "]");
}

static void describe(AEffect *e, const char *path, char *name, char *vendor) {
  get_str(e, effGetEffectName, 0, name); if (!*name) get_str(e, effGetProductString, 0, name);
  if (!*name) { const char *b = strrchr(path, '/'); copy_str(name, b ? b + 1 : path); char *dot = strrchr(name, '.'); if (dot) *dot = 0; }
  get_str(e, effGetVendorString, 0, vendor);
}

static void op_load(conn_t *c, const char *js) {                          // [main thread]
  if (!capture) { free(c->load_js); c->load_js = strdup(js); }
  char *path = jstr(js, "path"); double sr = jnum(js, "sampleRate", 48000);
  plug_unload(c);
  if (!path) { reply_err(c, js, "No plug-in file given"); return; }
  if (is_vst3_name(path)) {                                               // VST3
    char *pid = jstr(js, "id"); const char *err = v3_load(c, path, pid ? pid : "", sr); free(pid); free(path);
    if (err) { reply_err(c, js, err); return; }
    v3_t *v = V3OF(c); char hex[33] = ""; int events = V3(v->comp, v3_component, getBusCount, kEvent, kInput) > 0;
    sb_t s = reply_begin(js, 1); sb_str(&s, ",\"id\":"); sb_json(&s, hex); sb_str(&s, ",\"name\":"); sb_json(&s, c->name); sb_str(&s, ",\"vendor\":\"\"");
    sb_fmt(&s, ",\"latency\":%u,\"inputs\":%d,\"outputs\":%d,\"notes\":%s,\"state\":true,\"gui\":%s", V3(v->proc, v3_processor, getLatencySamples), v->nib ? v->ib[0].numChannels : 0, v->nob ? v->ob[0].numChannels : 0, events ? "true" : "false", v->ctrl ? "true" : "false");
    put_params(c, &s, 1); sb_str(&s, "}"); send_json(c, &s);
    atomic_store(&c->active, 1); return;
  }
  c->sr = sr; memset(&c->time, 0, sizeof c->time); c->time.sampleRate = sr; c->time.tempo = 120; c->time.sigNum = 4; c->time.sigDen = 4; c->time.flags = 1 << 1 | 1 << 10 | 1 << 13;   // playing, tempo and signature valid
  void *lib = NULL; AEffect *e = open_plugin(path, &lib, c);
  if (!e) { reply_err(c, js, HOST_BITS == 64 ? "This file could not be loaded as a 64-bit Windows VST2 plug-in" : "This file could not be loaded as a 32-bit Windows VST2 plug-in"); free(path); return; }
  char name[256], vendor[256]; describe(e, path, name, vendor); free(path);
  AcquireSRWLockExclusive(&load_lock);
  e->dispatcher(e, effSetSampleRate, 0, 0, NULL, (float)sr); e->dispatcher(e, effSetBlockSize, 0, BLOCK, NULL, 0);
  e->dispatcher(e, effMainsChanged, 0, 1, NULL, 0); e->dispatcher(e, effStartProcess, 0, 0, NULL, 0);
  ReleaseSRWLockExclusive(&load_lock);
  c->lib = lib; c->fx = e; c->started = 1; c->name = strdup(name); c->nin = e->numInputs; c->nout = e->numOutputs; c->in = make_bufs(c->nin); c->out = make_bufs(c->nout);
  int synth = (e->flags & flagIsSynth) != 0, notes = synth || e->dispatcher(e, effCanDo, 0, 0, "receiveVstMidiEvent", 0) > 0;
  sb_t s = reply_begin(js, 1);
  sb_fmt(&s, ",\"id\":\"%08x\",\"name\":", (unsigned)e->uniqueID); sb_json(&s, name); sb_str(&s, ",\"vendor\":"); sb_json(&s, vendor);
  sb_fmt(&s, ",\"latency\":%d,\"inputs\":%d,\"outputs\":%d,\"notes\":%s,\"state\":true,\"gui\":%s", e->initialDelay, synth ? 0 : e->numInputs, e->numOutputs, notes ? "true" : "false", e->flags & flagHasEditor ? "true" : "false");
  put_params(c, &s, 1); sb_str(&s, "}"); send_json(c, &s);
  atomic_store(&c->active, 1);
}

static void op_state(conn_t *c, const char *js) {                         // [main thread] "C"+chunk when the plug-in has one, else "P"+parameter values
  if (c->v3) {                                                            // VST3: see v3_state_save
    char *data = jstr(js, "data");
    if (data) {
      size_t n = strlen(data); uint8_t *raw = malloc(n + 1); size_t len = b64_dec(data, n, raw); free(data);
      int ok = v3_state_load(c, raw, len); free(raw); if (!ok) { reply_err(c, js, "The saved state does not fit this plug-in"); return; }
      sb_t s = reply_begin(js, 1); put_params(c, &s, 0); sb_str(&s, "}"); send_json(c, &s);
    } else { sb_t raw = { 0 }; v3_state_save(c, &raw); sb_t s = reply_begin(js, 1); sb_str(&s, ",\"data\":\""); b64_enc(&s, (const uint8_t *)raw.p, raw.n); sb_str(&s, "\"}"); free(raw.p); send_json(c, &s); }
    return;
  }
  AEffect *e = c->fx; if (!e) { reply_err(c, js, "No plug-in loaded"); return; }
  char *data = jstr(js, "data");
  if (data) {
    size_t n = strlen(data); uint8_t *raw = malloc(n + 1); size_t len = b64_dec(data, n, raw); free(data);
    if (len && raw[0] == 'C' && (e->flags & flagProgramChunks)) e->dispatcher(e, effSetChunk, 0, (intptr_t)(len - 1), raw + 1, 0);
    else if (len && raw[0] == 'P') { for (int32_t i = 0; i < e->numParams && 1 + (size_t)(i + 1) * 4 <= len; i++) { float v; memcpy(&v, raw + 1 + i * 4, 4); e->setParameter(e, i, v); } }
    else { free(raw); reply_err(c, js, "The saved state does not fit this plug-in"); return; }
    free(raw); sb_t s = reply_begin(js, 1); put_params(c, &s, 0); sb_str(&s, "}"); send_json(c, &s);
  } else {
    sb_t raw = { 0 };
    if (e->flags & flagProgramChunks) { void *chunk = NULL; intptr_t len = e->dispatcher(e, effGetChunk, 0, 0, &chunk, 0); sb_put(&raw, "C", 1); if (chunk && len > 0) sb_put(&raw, chunk, (size_t)len); }
    else { sb_put(&raw, "P", 1); for (int32_t i = 0; i < e->numParams; i++) { float v = e->getParameter(e, i); sb_put(&raw, &v, 4); } }
    sb_t s = reply_begin(js, 1); sb_str(&s, ",\"data\":\""); b64_enc(&s, (const uint8_t *)raw.p, raw.n); sb_str(&s, "\"}"); free(raw.p); send_json(c, &s);
  }
}

static int is_dll(const char *n) { size_t l = strlen(n); return l > 4 && n[l - 4] == '.' && (n[l - 3] | 32) == 'd' && (n[l - 2] | 32) == 'l' && (n[l - 1] | 32) == 'l'; }
static void scan_file(const char *unix_path, sb_t *s, int *first) {       // [main thread]
  { char *dp = dos_path(unix_path); int b = pe_bits(dp); free(dp); if (b != HOST_BITS) return; }   // the other host takes it
  void *lib = NULL; AEffect *e = open_plugin(unix_path, &lib, NULL); if (!e) return;
  char name[256], vendor[256]; describe(e, unix_path, name, vendor);
  sb_fmt(s, "%s{\"id\":\"%08x\",\"name\":", *first ? "" : ",", (unsigned)e->uniqueID); *first = 0; sb_json(s, name); sb_str(s, ",\"vendor\":"); sb_json(s, vendor);
  sb_str(s, ",\"version\":\"\",\"path\":"); sb_json(s, unix_path); sb_fmt(s, ",\"format\":\"vst2\",\"windows\":true,\"bits\":%d,\"instrument\":%s}", HOST_BITS, e->flags & flagIsSynth ? "true" : "false");
  AcquireSRWLockExclusive(&load_lock); e->dispatcher(e, effClose, 0, 0, NULL, 0); ReleaseSRWLockExclusive(&load_lock); FreeLibrary(lib);
}
static void scan_dir(const char *dir, int depth, sb_t *s, int *first) {
  size_t dl = strlen(dir); char *pat = malloc(dl + 4); memcpy(pat, dir, dl); memcpy(pat + dl, "/*", 3);
  char *dos = dos_path(pat); FIND_DATA *fd = malloc(sizeof *fd); HANDLE h = FindFirstFileA(dos, fd); free(dos); free(pat);
  if (h != (HANDLE)(intptr_t)-1) {
    do {
      if (fd->name[0] == '.') continue;
      size_t nl = strlen(fd->name); char *p = malloc(dl + nl + 2); memcpy(p, dir, dl); p[dl] = '/'; memcpy(p + dl + 1, fd->name, nl + 1);
      if (is_vst3_name(fd->name)) v3_scan(p, s, first);                 // a .vst3 file or bundle folder
      else if (fd->attr & 0x10) { if (depth < 4) scan_dir(p, depth + 1, s, first); } else if (is_dll(fd->name)) scan_file(p, s, first);
      free(p);
    } while (FindNextFileA(h, fd));
    FindClose(h);
  }
  free(fd);
}
static void op_scan(conn_t *c, const char *js) {
  sb_t s = reply_begin(js, 1); int first = 1; sb_str(&s, ",\"plugins\":[");
  const char *a = jget(js, "paths");
  if (a && *a == '[') for (const char *p = jws(a + 1); *p == '"';) { char *dir = jstr_at(p); if (dir) { scan_dir(dir, 0, &s, &first); free(dir); } p = jws(jskip(p)); if (*p == ',') p = jws(p + 1); }
  sb_str(&s, "]}"); send_json(c, &s);
}

static void run_job(conn_t *c, const char *js) {                          // [main thread]
  char *op = jstr(js, "op"); if (!op) return;
  if (c->v3) v3_sync_ctl(c);                                              // parameter values the app sent go to the controller first
  if (!strcmp(op, "hello")) { sb_t s = reply_begin(js, 1); sb_fmt(&s, ",\"host\":\"undae-vsthost\",\"version\":1,\"bits\":%d,\"formats\":[\"vst2\",\"vst3\"],\"gui\":true}", HOST_BITS); send_json(c, &s); }
  else if (!strcmp(op, "scan")) op_scan(c, js);
  else if (!strcmp(op, "load")) op_load(c, js);
  else if (!strcmp(op, "panic")) {
    // Transport stopped: silence an instrument at once, release tails included. Many plug-ins keep sounding through a
    // reset, so the instance is replaced: its state is saved, it is loaded afresh, and the state is put back.
    if ((c->fx || c->v3) && c->load_js) {
      sb_t got = { 0 }; int had_window = c->hwnd != NULL;
      capture = &got; op_state(c, "{\"op\":\"state\"}"); capture = NULL;
      char *data = got.p ? jstr(got.p, "data") : NULL; free(got.p);
      char *again = strdup(c->load_js); got = (sb_t){ 0 }; capture = &got; op_load(c, again); free(again);
      if (data && (c->fx || c->v3)) { sb_t js = { 0 }; sb_str(&js, "{\"op\":\"state\",\"data\":"); sb_json(&js, data); sb_str(&js, "}"); op_state(c, js.p); free(js.p); }
      capture = NULL; free(got.p); free(data);
      EnterCriticalSection(&c->qlock); c->nmidi = 0; c->npq = 0; LeaveCriticalSection(&c->qlock);   // notes queued ahead must not play on the new instance
      if (had_window && (c->fx || c->v3)) gui_show(c);
    }
  }
  else if (!strcmp(op, "unload")) { plug_unload(c); sb_t s = reply_begin(js, 1); sb_str(&s, "}"); send_json(c, &s); }
  else if (!strcmp(op, "params")) { if (c->fx || c->v3) { sb_t s = reply_begin(js, 1); put_params(c, &s, 0); sb_str(&s, "}"); send_json(c, &s); } else reply_err(c, js, "No plug-in loaded"); }
  else if (!strcmp(op, "state")) op_state(c, js);
  else if (!strcmp(op, "gui")) {
    const char *err = NULL; if (jtrue(js, "show")) err = gui_show(c); else gui_close(c);
    if (err) reply_err(c, js, err); else { sb_t s = reply_begin(js, 1); sb_fmt(&s, ",\"open\":%s}", c->hwnd ? "true" : "false"); send_json(c, &s); }
  }
  else reply_err(c, js, "Unknown request");
  free(op);
}

// ---------------------------------------------------------------------------------------------- audio thread
static void process_block(conn_t *c, const uint8_t *msg, size_t len, float *out) {
  uint32_t head[3]; memcpy(head, msg, 12); uint32_t n = head[2];
  if (head[0] != 1 || !n || n > MAXF || len < 12 + (size_t)n * 8) return;
  const float *in = (const float *)(msg + 12); float *o = out + 3; memcpy(out, head, 12);
  atomic_store(&c->busy, 1);
  if (!atomic_load(&c->active)) { atomic_store(&c->busy, 0); memcpy(o, in, (size_t)n * 8); ws_send(c, 2, out, 12 + (size_t)n * 8); return; }
  if (c->v3) {
    AcquireSRWLockShared(&load_lock); v3_process_block(c, head[1], n, in, o); ReleaseSRWLockShared(&load_lock); atomic_store(&c->busy, 0);
    ws_send(c, 2, out, 12 + (size_t)n * 8); return;
  }
  AEffect *e = c->fx; AcquireSRWLockShared(&load_lock);
  EnterCriticalSection(&c->qlock); int nm = 0, keep = 0; uint32_t pos = head[1];   // take the notes that fall inside this block, in time order
  for (int i = 0; i < c->nmidi; i++) {
    int32_t off = c->mtimed[i] ? (int32_t)(c->mat[i] - pos) : 0;
    if (off >= (int32_t)n) { c->midi[keep] = c->midi[i]; c->mat[keep] = c->mat[i]; c->mtimed[keep] = c->mtimed[i]; keep++; continue; }
    VstMidiEvent m = c->midi[i]; m.deltaFrames = off < 0 ? 0 : off; int k = nm++; while (k && c->cur[k - 1].deltaFrames > m.deltaFrames) { c->cur[k] = c->cur[k - 1]; k--; } c->cur[k] = m;
  }
  c->nmidi = keep; LeaveCriticalSection(&c->qlock);
  int np = 0, pkeep = 0;                                                    // parameter changes due in this block, in time order
  EnterCriticalSection(&c->qlock);
  for (int i = 0; i < c->npq; i++) {
    int32_t off = (int32_t)(c->pq[i].at - pos); if (off >= (int32_t)n) { c->pq[pkeep++] = c->pq[i]; continue; }
    if (off < 0) off = 0;
    int k = np++; while (k && (int32_t)c->pcur[k - 1].at > off) { c->pcur[k] = c->pcur[k - 1]; k--; } c->pcur[k] = c->pq[i]; c->pcur[k].at = (uint32_t)off;
  }
  c->npq = pkeep; LeaveCriticalSection(&c->qlock);
  if (c->nin >= 2) for (uint32_t i = 0; i < n; i++) { c->in[0][i] = in[i * 2]; c->in[1][i] = in[i * 2 + 1]; }
  else if (c->nin == 1) for (uint32_t i = 0; i < n; i++) c->in[0][i] = (in[i * 2] + in[i * 2 + 1]) * 0.5f;
  float *ip[64], *op[64]; int ni = c->nin > 64 ? 64 : c->nin, no = c->nout > 64 ? 64 : c->nout, mi = 0, pi = 0;
  for (uint32_t off = 0; off < n;) {                                        // process in pieces that end where a parameter changes
    while (pi < np && c->pcur[pi].at <= off) { if (c->pcur[pi].id >= 0 && c->pcur[pi].id < e->numParams) e->setParameter(e, c->pcur[pi].id, c->pcur[pi].v); pi++; }
    uint32_t end = off + BLOCK < n ? off + BLOCK : n; if (pi < np && c->pcur[pi].at < end) end = c->pcur[pi].at;
    int k0 = mi; while (mi < nm && (uint32_t)c->cur[mi].deltaFrames < end) { c->cur[mi].deltaFrames -= (int32_t)off; mi++; }   // notes for this piece, relative to it
    if (mi > k0) { c->events.numEvents = mi - k0; c->events.reserved = 0; for (int i = k0; i < mi; i++) c->events.events[i - k0] = &c->cur[i]; e->dispatcher(e, effProcessEvents, 0, 0, &c->events, 0); }
    int32_t k = (int32_t)(end - off);
    for (int i = 0; i < ni; i++) ip[i] = c->in[i] + off;
    for (int i = 0; i < no; i++) op[i] = c->out[i] + off;
    if ((e->flags & flagCanReplacing) && e->processReplacing) e->processReplacing(e, ip, op, k);
    else { for (int i = 0; i < no; i++) memset(op[i], 0, (size_t)k * 4); if (e->process) e->process(e, ip, op, k); }
    c->time.samplePos += k; off = end;
  }
  ReleaseSRWLockShared(&load_lock); atomic_store(&c->busy, 0);
  if (!no) memset(o, 0, (size_t)n * 8);
  else { const float *L = c->out[0], *R = no > 1 ? c->out[1] : L; for (uint32_t i = 0; i < n; i++) { float l = L[i], r = R[i]; o[i * 2] = l > -64 && l < 64 ? l : 0; o[i * 2 + 1] = r > -64 && r < 64 ? r : 0; } }   // drops NaN, infinity and runaway values
  ws_send(c, 2, out, 12 + (size_t)n * 8);
}

static void queue_midi(conn_t *c, const uint8_t *d, const char *js) {   // `at`: the audio frame it belongs to; none = next block
  VstMidiEvent m; memset(&m, 0, sizeof m); m.type = 1; m.byteSize = (int32_t)sizeof m; memcpy(m.midi, d, 3);
  const char *at = jget(js, "at");
  EnterCriticalSection(&c->qlock); if (c->nmidi < MAXQ) { c->midi[c->nmidi] = m; c->mtimed[c->nmidi] = at != NULL; c->mat[c->nmidi] = at ? frame_of(at) : 0; c->nmidi++; } LeaveCriticalSection(&c->qlock);
}
static void op_note(conn_t *c, const char *js) {
  int on = jtrue(js, "on"), key = (int)jnum(js, "key", 60); double vel = jnum(js, "vel", 0.8); int v = (int)(vel * 127); if (v < 1) v = 1; if (v > 127) v = 127;
  uint8_t d[3] = { (uint8_t)(on ? 0x90 : 0x80), (uint8_t)(key & 127), (uint8_t)(on ? v : 0) }; queue_midi(c, d, js);
}
static void op_midi(conn_t *c, const char *js) {                          // pitch bend, mod wheel, sustain and other raw MIDI
  const char *p = jget(js, "data"); uint8_t d[3] = { 0 }; int n = 0; if (!p || *p != '[') return;
  for (p = jws(p + 1); n < 3 && *p >= '0' && *p <= '9'; n++) { d[n] = (uint8_t)strtod(p, NULL); p = jws(jskip(p)); if (*p == ',') p = jws(p + 1); }
  if (n) queue_midi(c, d, js);
}

static uint32_t WINAPI reader(void *arg) {
  conn_t *c = arg; sb_t msg = { 0 }; int msg_op = 0; float *out = malloc((3 + MAXF * 2) * sizeof(float));
  SetThreadPriority(GetCurrentThread(), 15);                             // this is the audio thread
  char *req = calloc(1, 8192); size_t rn = 0; int ok = 0;
  while (rn < 8191) { int k = recv(c->fd, req + rn, (int)(8191 - rn), 0); if (k <= 0) break; rn += (size_t)k; req[rn] = 0; if (strstr(req, "\r\n\r\n")) { ok = 1; break; } }
  if (ok) { char *end = strstr(req, "\r\n\r\n") + 4; c->rlen = rn - (size_t)(end - req); memcpy(c->rbuf, end, c->rlen); }
  if (ok) { sb_t s = { 0 }; ok = ws_accept(req, token, &s); if (ok) ok = write_all(c->fd, s.p, s.n); else write_all(c->fd, "HTTP/1.1 403 Forbidden\r\n\r\n", 26); free(s.p); }
  free(req);
  while (ok) {
    uint8_t h[2], ext[8], mask[4]; if (!read_all(c, h, 2)) break;
    int fin = h[0] & 0x80, op = h[0] & 15; uint64_t n = h[1] & 127;
    if (n == 126) { if (!read_all(c, ext, 2)) break; n = (uint64_t)ext[0] << 8 | ext[1]; }
    else if (n == 127) { if (!read_all(c, ext, 8)) break; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | ext[i]; }
    if (!(h[1] & 0x80) || n > MAXMSG || msg.n + n > MAXMSG) break;
    if (!read_all(c, mask, 4)) break;
    size_t at = msg.n; if (msg.cap < at + n + 1) { msg.cap = (at + n + 1) * 2; msg.p = realloc(msg.p, msg.cap); }
    if (n && !read_all(c, msg.p + at, n)) break;
    for (uint64_t i = 0; i < n; i++) msg.p[at + i] ^= mask[i & 3];
    if (op == 8) break;
    if (op == 9) { ws_send(c, 10, msg.p + at, n); continue; }
    if (op == 10) continue;
    if (op) msg_op = op;
    msg.n = at + n; msg.p[msg.n] = 0;
    if (!fin) continue;
    if (msg_op == 2) {                                                  // UNDAE_SERIAL=1: one plug-in processes at a time (multi-threaded processing off)
      if (serial) EnterCriticalSection(&serial_lock);
      process_block(c, (const uint8_t *)msg.p, msg.n, out);
      if (serial) LeaveCriticalSection(&serial_lock);
    }
    else if (msg_op == 1) {
      char *name = jstr(msg.p, "op");
      if (name && (!strcmp(name, "param") || !strcmp(name, "note") || !strcmp(name, "midi"))) {
        atomic_store(&c->busy, 1);
        if (atomic_load(&c->active)) { if (name[0] == 'n') op_note(c, msg.p); else if (name[0] == 'm') op_midi(c, msg.p); else {
          int32_t id = (int32_t)jnum(msg.p, "id", -1); float v = (float)jnum(msg.p, "value", 0); const char *at = jget(msg.p, "at");
          if (at || c->v3) {                                              // VST3 takes every change through process(); VST2 only timed ones
            EnterCriticalSection(&c->qlock);
            if (c->npq < MAXQ) { c->pq[c->npq].at = at ? frame_of(at) : 0; c->pq[c->npq].timed = at != NULL; c->pq[c->npq].id = id; c->pq[c->npq].v = v; c->npq++; }
            if (c->v3 && !at && V3OF(c)->nto_ctl < 256) { V3OF(c)->to_ctl[V3OF(c)->nto_ctl].id = (ParamID)id; V3OF(c)->to_ctl[V3OF(c)->nto_ctl].v = v; V3OF(c)->nto_ctl++; }   // so its window shows it
            LeaveCriticalSection(&c->qlock);
          }
          else if (id >= 0 && id < c->fx->numParams) c->fx->setParameter(c->fx, id, v);
        } }
        atomic_store(&c->busy, 0);
      } else { job_t *j = calloc(1, sizeof *j); j->c = c; j->text = strdup(msg.p); EnterCriticalSection(&mlock); if (jobs_tail) jobs_tail->next = j; else jobs = j; jobs_tail = j; LeaveCriticalSection(&mlock); wake(); }
      free(name);
    }
    msg.n = 0;
  }
  free(msg.p); free(out); shutdown(c->fd, 2);
  atomic_store(&c->dead, 1); wake();
  return 0;
}

static uint32_t WINAPI acceptor(void *arg) {
  SOCKET ls = (SOCKET)(uintptr_t)arg;
  for (;;) {
    SOCKET fd = accept(ls, NULL, NULL); if (fd == (SOCKET)-1) break;
    int32_t one = 1; setsockopt(fd, 6, 1, (const char *)&one, 4);         // TCP_NODELAY
    conn_t *c = calloc(1, sizeof *c); c->fd = fd; c->rbuf = malloc(RBUF); c->wbuf = malloc(RBUF); InitializeCriticalSection(&c->wlock); InitializeCriticalSection(&c->qlock);
    EnterCriticalSection(&mlock); c->next = conns; conns = c; LeaveCriticalSection(&mlock);
    c->thread = CreateThread(NULL, 1 << 20, reader, c, 0, NULL);
  }
  return 0;
}

static uint32_t WINAPI watch_parent(void *arg) {                                 // the app holds our stdin open; when it goes, so do we
  (void)arg; char b[64]; uint32_t got;
  while (ReadFile(GetStdHandle((uint32_t)-10), b, sizeof b, &got, NULL) && got) { }
  ExitProcess(0); return 0;
}

void entry(void) {
  SetErrorMode(0x8003); main_tid = GetCurrentThreadId(); InitializeCriticalSection(&mlock); InitializeCriticalSection(&serial_lock);
  { const char *e = getenv("UNDAE_SERIAL"); serial = e && *e == '1'; } wake_ev = CreateEventA(NULL, 0, 0, NULL);
  const char *env = getenv("UNDAE_HOST_TOKEN");
  if (env && *env && strlen(env) < sizeof token) copy_str(token, env);
  else { uint8_t r[16]; SystemFunction036(r, 16); for (int i = 0; i < 16; i++) _snprintf(token + i * 2, 3, "%02x", r[i]); }
  char wsa[512]; WSAStartup(0x0202, wsa);
  SOCKET ls = socket(2, 1, 6); struct sockaddr_in a; memset(&a, 0, sizeof a); a.family = 2; a.addr = 0x0100007f; int al = sizeof a;   // 127.0.0.1, any port
  if (bind(ls, &a, sizeof a) || listen(ls, 16) || getsockname(ls, &a, &al)) { printf("{\"error\":\"listen failed\"}\n"); fflush(NULL); ExitProcess(1); }
  printf("{\"port\":%d,\"token\":\"%s\"}\n", (a.port >> 8 | (a.port & 255) << 8) & 0xffff, token); fflush(NULL);
  if (strstr(GetCommandLineA(), "--child")) CreateThread(NULL, 0, watch_parent, NULL, 0, NULL);
  CreateThread(NULL, 0, acceptor, (void *)(uintptr_t)ls, 0, NULL);

  for (;;) {
    MsgWaitForMultipleObjects(1, &wake_ev, 0, 50, 0x04FF);                 // a wake-up, a window message, or 50 ms
    for (MSG m; PeekMessageA(&m, NULL, 0, 0, 1);) { TranslateMessage(&m); DispatchMessageA(&m); }
    EnterCriticalSection(&mlock);
    while (jobs) {
      job_t *j = jobs; jobs = j->next; if (!jobs) jobs_tail = NULL;
      LeaveCriticalSection(&mlock); if (!atomic_load(&j->c->dead)) run_job(j->c, j->text); free(j->text); free(j); EnterCriticalSection(&mlock);
    }
    for (conn_t **pc = &conns; *pc && !jobs;) {
      conn_t *c = *pc;
      if (atomic_load(&c->dead)) { *pc = c->next; LeaveCriticalSection(&mlock); WaitForSingleObject(c->thread, 0xffffffff); CloseHandle(c->thread); plug_unload(c); closesocket(c->fd); free(c->rbuf); free(c->wbuf); free(c->load_js); free(c); EnterCriticalSection(&mlock); pc = &conns; continue; }
      if (c->v3 && V3OF(c)->nto_ctl) { LeaveCriticalSection(&mlock); v3_sync_ctl(c); EnterCriticalSection(&mlock); pc = &conns; continue; }   // values from the app, shown in the controller
      if ((c->fx || c->v3) && atomic_exchange(&c->want_rescan, 0)) { LeaveCriticalSection(&mlock); sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"params\""); put_params(c, &s, 1); sb_str(&s, "}"); send_json(c, &s); EnterCriticalSection(&mlock); pc = &conns; continue; }
      if ((c->fx || c->v3) && atomic_exchange(&c->want_out, 0)) {
        LeaveCriticalSection(&mlock);
        struct { int32_t id; float value; } ev[64]; EnterCriticalSection(&c->qlock); int ne = c->nout_ev; memcpy(ev, c->out_ev, sizeof ev); c->nout_ev = 0; LeaveCriticalSection(&c->qlock);
        sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"param\",\"changes\":[");
        for (int i = 0; i < ne; i++) { char disp[300]; if (c->v3) v3_text(c, (ParamID)ev[i].id, ev[i].value, disp, sizeof disp); else { get_str(c->fx, effGetParamDisplay, ev[i].id, disp); trim(disp); } sb_fmt(&s, "%s{\"id\":%d,\"value\":", i ? "," : "", ev[i].id); sb_num(&s, ev[i].value); sb_str(&s, ",\"text\":"); sb_json(&s, disp); sb_str(&s, "}"); }
        sb_str(&s, "]}"); send_json(c, &s); EnterCriticalSection(&mlock); pc = &conns; continue;
      }
      if (atomic_exchange(&c->want_closed, 0)) { LeaveCriticalSection(&mlock); sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"gui\",\"open\":false}"); send_json(c, &s); EnterCriticalSection(&mlock); pc = &conns; continue; }
      if ((c->fx || c->v3) && atomic_exchange(&c->want_dirty, 0)) { LeaveCriticalSection(&mlock); sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"dirty\"}"); send_json(c, &s); EnterCriticalSection(&mlock); pc = &conns; continue; }
      pc = &c->next;
    }
    LeaveCriticalSection(&mlock);
  }
}
