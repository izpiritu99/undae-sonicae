// VST3 plug-ins in the Windows host (vsthost.c includes this after its own helpers). A VST3 plug-in is a component
// (audio processing) and an edit controller (parameters, window), sometimes one object; the host connects the two,
// copies the component's state into the controller, and feeds the processor audio, note events and parameter changes.
// The interface declarations are in ../vst3c.h.
#define V3_WINDOWS
#if defined(__x86_64__)
#define PAPI
#else
#define PAPI __attribute__((stdcall))
#endif
#include "../vst3c.h"
#include "../vst3host.h"
// ---------------------------------------------------------------------------------------------- a loaded plug-in
typedef struct { const struct v3_handler_vt *vt; conn_t *c; } v3_handler;
typedef struct { const struct v3_frame_vt *vt; conn_t *c; } v3_frame;
typedef struct v3 {
  void *lib, *factory, *comp, *proc, *ctrl, *cp_comp, *cp_ctrl, *view; int single;
  v3_handler handler; v3_frame frame;
  int nib, nob; AudioBusBuffers *ib, *ob;
  v3_changes pin, pout; v3_events ev; ProcessContext ctx; ProcessData pd;
  int32_t midimap[130];                                                   // parameter that a MIDI controller drives, or -1
  struct { ParamID id; double v; } to_ctl[256]; int nto_ctl;              // values the controller should show (main thread applies them)
} v3_t;
#define V3OF(c) ((v3_t *)(c)->v3)

static void report_change(conn_t *c, ParamID id, double v) {             // a change the plug-in made: tell the app, mark the state dirty
  EnterCriticalSection(&c->qlock); if (c->nout_ev < 64) { c->out_ev[c->nout_ev].id = (int32_t)id; c->out_ev[c->nout_ev].value = (float)v; c->nout_ev++; } LeaveCriticalSection(&c->qlock);
  atomic_store(&c->want_out, 1); atomic_store(&c->want_dirty, 1); wake();
}
static void to_processor(conn_t *c, ParamID id, double v) {             // an untimed change for the processor, at the next block
  EnterCriticalSection(&c->qlock); if (c->npq < MAXQ) { c->pq[c->npq].at = 0; c->pq[c->npq].timed = 0; c->pq[c->npq].id = (int32_t)id; c->pq[c->npq].v = (float)v; c->npq++; } LeaveCriticalSection(&c->qlock);
}
static tresult PAPI hd_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IComponentHandler)) { *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static tresult PAPI hd_begin(void *self, ParamID id) { (void)self; (void)id; return kResultOk; }
static tresult PAPI hd_perform(void *self, ParamID id, ParamValue v) { conn_t *c = ((v3_handler *)self)->c; to_processor(c, id, v); report_change(c, id, v); return kResultOk; }
static tresult PAPI hd_end(void *self, ParamID id) { (void)id; atomic_store(&((v3_handler *)self)->c->want_dirty, 1); wake(); return kResultOk; }
static tresult PAPI hd_restart(void *self, int32_t flags) { if (flags & (1 << 2 | 1 << 4)) { atomic_store(&((v3_handler *)self)->c->want_rescan, 1); wake(); } return kResultOk; }   // parameter values or titles changed
static const struct v3_handler_vt hd_vt = { hd_qi, ref_one, ref_one, hd_begin, hd_perform, hd_end, hd_restart };
static tresult PAPI fr_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IPlugFrame)) { *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static tresult PAPI fr_resize(void *self, void *view, ViewRect *r) {
  conn_t *c = ((v3_frame *)self)->c; if (!c->hwnd || !r) return kResultFalse;
  size_window(c, r->right - r->left, r->bottom - r->top); V3(view, v3_view, onSize, r); return kResultOk;
}
static const struct v3_frame_vt fr_vt = { fr_qi, ref_one, ref_one, fr_resize };

static int is_vst3_name(const char *n) { size_t l = strlen(n); return l > 5 && n[l - 5] == '.' && (n[l - 4] | 32) == 'v' && (n[l - 3] | 32) == 's' && (n[l - 2] | 32) == 't' && n[l - 1] == '3'; }
static int is_dir(const char *unix_path) { char *d = dos_path(unix_path); FIND_DATA *f = malloc(sizeof *f); HANDLE h = FindFirstFileA(d, f); int r = h != (HANDLE)(intptr_t)-1 && (f->attr & 0x10); if (h != (HANDLE)(intptr_t)-1) FindClose(h); free(f); free(d); return r; }
// The DLL inside a .vst3: the path itself (a single file), or Contents/<arch>-win/<name>.vst3 inside a bundle folder.
static char *v3_binary(const char *unix_path) {
  if (!is_dir(unix_path)) return strdup(unix_path);
  const char *base = strrchr(unix_path, '/'); base = base ? base + 1 : unix_path;
  size_t n = strlen(unix_path) + strlen(base) + 40; char *p = malloc(n);
  _snprintf(p, n, "%s/Contents/%s/%s", unix_path, HOST_BITS == 64 ? "x86_64-win" : "x86-win", base); p[n - 1] = 0; return p;
}
static void *v3_module(const char *binary, void **lib) {                 // load the DLL and return its factory
  char *d = dos_path(binary); int bits = pe_bits(d); void *h = bits == HOST_BITS ? LoadLibraryA(d) : NULL; free(d); if (!h) return NULL;
  uint8_t (*init)(void) = GetProcAddress(h, "InitDll");                   // returns a C++ bool; cdecl
  void *(PAPI *getf)(void) = GetProcAddress(h, "GetPluginFactory");
  if (init && !init()) { FreeLibrary(h); return NULL; }
  void *f = getf ? getf() : NULL; if (!f) { uint8_t (*fin)(void) = GetProcAddress(h, "ExitDll"); if (fin) fin(); FreeLibrary(h); return NULL; }
  *lib = h; return f;
}
static void v3_module_close(void *lib, void *factory) { if (factory) V3(factory, v3_unknown, release); uint8_t (*fin)(void) = GetProcAddress(lib, "ExitDll"); if (fin) fin(); FreeLibrary(lib); }

static void v3_scan(const char *unix_path, sb_t *s, int *first) {        // [main thread] list the audio classes in a .vst3
  char *bin = v3_binary(unix_path); void *lib = NULL; AcquireSRWLockExclusive(&load_lock); void *f = v3_module(bin, &lib); ReleaseSRWLockExclusive(&load_lock); free(bin); if (!f) return;
  PFactoryInfo fi; memset(&fi, 0, sizeof fi); V3(f, v3_factory, getFactoryInfo, &fi);
  void *f2 = NULL; if (V3(f, v3_unknown, queryInterface, IID_IPluginFactory2, &f2) != kResultOk) f2 = NULL;
  for (int32_t i = 0, n = V3(f, v3_factory, countClasses); i < n; i++) {
    PClassInfo2 ci; memset(&ci, 0, sizeof ci);
    if (f2) { if (V3(f2, v3_factory, getClassInfo2, i, &ci) != kResultOk) continue; }
    else if (V3(f, v3_factory, getClassInfo, i, (PClassInfo *)&ci) != kResultOk) continue;
    if (strcmp(ci.category, "Audio Module Class")) continue;
    char hex[33]; hex_cid(ci.cid, hex); ci.name[63] = 0; ci.vendor[63] = 0; ci.version[63] = 0; fi.vendor[63] = 0;
    sb_fmt(s, "%s{\"id\":\"%s\",\"name\":", *first ? "" : ",", hex); *first = 0; sb_json(s, ci.name); sb_str(s, ",\"vendor\":"); sb_json(s, *ci.vendor ? ci.vendor : fi.vendor);
    sb_str(s, ",\"version\":"); sb_json(s, ci.version); sb_str(s, ",\"path\":"); sb_json(s, unix_path);
    sb_fmt(s, ",\"format\":\"vst3\",\"windows\":true,\"bits\":%d,\"instrument\":%s}", HOST_BITS, strstr(ci.subCategories, "Instrument") ? "true" : "false");
  }
  if (f2) V3(f2, v3_unknown, release);
  AcquireSRWLockExclusive(&load_lock); v3_module_close(lib, f); ReleaseSRWLockExclusive(&load_lock);
}

static void free_busbufs(AudioBusBuffers *b, int n) { if (!b) return; for (int i = 0; i < n; i++) { for (int k = 0; k < b[i].numChannels; k++) free(b[i].channelBuffers32[k]); free(b[i].channelBuffers32); } free(b); }
static AudioBusBuffers *make_busbufs(void *comp, int dir, int n) {
  AudioBusBuffers *b = calloc((size_t)(n ? n : 1), sizeof *b);
  for (int i = 0; i < n; i++) {
    BusInfo bi; memset(&bi, 0, sizeof bi); V3(comp, v3_component, getBusInfo, kAudio, dir, i, &bi);
    int ch = bi.channelCount > 0 && bi.channelCount <= 32 ? bi.channelCount : 2;
    b[i].numChannels = ch; b[i].channelBuffers32 = calloc((size_t)ch, sizeof(float *)); for (int k = 0; k < ch; k++) b[i].channelBuffers32[k] = calloc(MAXF, sizeof(float));
  }
  return b;
}

static void v3_unload(conn_t *c) {                                        // [main thread]
  v3_t *v = V3OF(c); if (!v) return;
  atomic_store(&c->active, 0); while (atomic_load(&c->busy)) Sleep(1);
  AcquireSRWLockExclusive(&load_lock);
  if (v->proc) V3(v->proc, v3_processor, setProcessing, 0);
  V3(v->comp, v3_component, setActive, 0);
  if (v->cp_comp && v->cp_ctrl) { V3(v->cp_comp, v3_connection, disconnect, v->cp_ctrl); V3(v->cp_ctrl, v3_connection, disconnect, v->cp_comp); }
  if (v->cp_comp) V3(v->cp_comp, v3_unknown, release);
  if (v->cp_ctrl) V3(v->cp_ctrl, v3_unknown, release);
  if (v->ctrl) { V3(v->ctrl, v3_controller, setComponentHandler, NULL); if (!v->single) V3(v->ctrl, v3_pluginbase, terminate); V3(v->ctrl, v3_unknown, release); }
  if (v->proc) V3(v->proc, v3_unknown, release);
  V3(v->comp, v3_pluginbase, terminate); V3(v->comp, v3_unknown, release);
  v3_module_close(v->lib, v->factory);
  ReleaseSRWLockExclusive(&load_lock);
  free_busbufs(v->ib, v->nib); free_busbufs(v->ob, v->nob); free(v); c->v3 = NULL; free(c->name); c->name = NULL;
}

// [main thread] Load a VST3 class (`cid` hex, or empty for the first audio class). NULL on success, else why not.
static const char *v3_load(conn_t *c, const char *path, const char *cidhex, double sr) {
  char *bin = v3_binary(path); void *lib = NULL; v3_t *v = calloc(1, sizeof *v); const char *err = NULL;
  AcquireSRWLockExclusive(&load_lock);
  void *f = v3_module(bin, &lib); free(bin);
  if (!f) { ReleaseSRWLockExclusive(&load_lock); free(v); return HOST_BITS == 64 ? "This file could not be loaded as a 64-bit Windows VST3 plug-in" : "This file could not be loaded as a 32-bit Windows VST3 plug-in"; }
  v->lib = lib; v->factory = f;
  uint8_t want[16]; int pick = parse_cid(cidhex, want); PClassInfo ci; int found = 0;
  for (int32_t i = 0, n = V3(f, v3_factory, countClasses); i < n && !found; i++) { memset(&ci, 0, sizeof ci); if (V3(f, v3_factory, getClassInfo, i, &ci) == kResultOk && !strcmp(ci.category, "Audio Module Class") && (!pick || same_iid(ci.cid, want))) found = 1; }
  if (!found || V3(f, v3_factory, createInstance, ci.cid, IID_IComponent, &v->comp) != kResultOk || !v->comp) { err = "That plug-in is not in the file"; goto fail; }
  if (V3(v->comp, v3_pluginbase, initialize, &host_app) != kResultOk) { V3(v->comp, v3_unknown, release); v->comp = NULL; err = "The plug-in failed to initialise"; goto fail; }
  if (V3(v->comp, v3_unknown, queryInterface, IID_IAudioProcessor, &v->proc) != kResultOk) v->proc = NULL;
  if (!v->proc) { err = "The plug-in has no audio processor"; goto fail_comp; }
  if (V3(v->comp, v3_unknown, queryInterface, IID_IEditController, &v->ctrl) == kResultOk && v->ctrl) v->single = 1;
  else {
    TUID ccid; v->ctrl = NULL;
    if (V3(v->comp, v3_component, getControllerClassId, ccid) == kResultOk && V3(f, v3_factory, createInstance, ccid, IID_IEditController, &v->ctrl) == kResultOk && v->ctrl) {
      if (V3(v->ctrl, v3_pluginbase, initialize, &host_app) != kResultOk) { V3(v->ctrl, v3_unknown, release); v->ctrl = NULL; }
    } else v->ctrl = NULL;
  }
  if (v->ctrl && !v->single) {                                            // let the two halves talk, and give the controller the component's state
    if (V3(v->comp, v3_unknown, queryInterface, IID_IConnectionPoint, &v->cp_comp) != kResultOk) v->cp_comp = NULL;
    if (V3(v->ctrl, v3_unknown, queryInterface, IID_IConnectionPoint, &v->cp_ctrl) != kResultOk) v->cp_ctrl = NULL;
    if (v->cp_comp && v->cp_ctrl) { V3(v->cp_comp, v3_connection, connect, v->cp_ctrl); V3(v->cp_ctrl, v3_connection, connect, v->cp_comp); }
    v3_stream *st = stream_new(NULL, 0);
    if (V3(v->comp, v3_component, getState, st) == kResultOk) { st->pos = 0; V3(v->ctrl, v3_controller, setComponentState, st); }
    v3st_rel(st);
  }
  v->handler.vt = &hd_vt; v->handler.c = c; v->frame.vt = &fr_vt; v->frame.c = c;
  if (v->ctrl) V3(v->ctrl, v3_controller, setComponentHandler, &v->handler);
  for (int k = 0; k < 130; k++) v->midimap[k] = -1;
  void *mm = NULL;
  if (v->ctrl && V3(v->ctrl, v3_unknown, queryInterface, IID_IMidiMapping, &mm) == kResultOk && mm) {
    for (int k = 0; k < 130; k++) { ParamID id; if (V3(mm, v3_midimapping, getMidiControllerAssignment, 0, 0, (int16_t)k, &id) == kResultOk) v->midimap[k] = (int32_t)id; }
    V3(mm, v3_unknown, release);
  }
  v->nib = V3(v->comp, v3_component, getBusCount, kAudio, kInput); v->nob = V3(v->comp, v3_component, getBusCount, kAudio, kOutput);
  if (v->nib > 16) v->nib = 16;
  if (v->nob > 16) v->nob = 16;
  {                                                                       // main buses stereo, the rest as the plug-in has them
    uint64_t ia[16], oa[16];
    for (int i = 0; i < v->nib; i++) { ia[i] = V3_SPEAKERS_STEREO; if (i) V3(v->proc, v3_processor, getBusArrangement, kInput, i, &ia[i]); }
    for (int i = 0; i < v->nob; i++) { oa[i] = V3_SPEAKERS_STEREO; if (i) V3(v->proc, v3_processor, getBusArrangement, kOutput, i, &oa[i]); }
    V3(v->proc, v3_processor, setBusArrangements, ia, v->nib, oa, v->nob);
  }
  v->ib = make_busbufs(v->comp, kInput, v->nib); v->ob = make_busbufs(v->comp, kOutput, v->nob);
  if (v->nib) V3(v->comp, v3_component, activateBus, kAudio, kInput, 0, 1);
  if (v->nob) V3(v->comp, v3_component, activateBus, kAudio, kOutput, 0, 1);
  if (V3(v->comp, v3_component, getBusCount, kEvent, kInput) > 0) V3(v->comp, v3_component, activateBus, kEvent, kInput, 0, 1);
  ProcessSetup ps = { 0, 0, BLOCK, sr };
  if (V3(v->proc, v3_processor, setupProcessing, &ps) != kResultOk) { err = "The plug-in cannot run at this sample rate"; goto fail_all; }
  V3(v->comp, v3_component, setActive, 1); V3(v->proc, v3_processor, setProcessing, 1);
  v->pin.vt = &v3pc_vt; v->pout.vt = &v3pc_vt; v->ev.vt = &v3ev_vt;
  v->ctx.state = kCtxPlaying | kCtxTempoValid | kCtxTimeSigValid | kCtxProjectTimeMusicValid; v->ctx.sampleRate = sr; v->ctx.tempo = 120; v->ctx.timeSigNumerator = 4; v->ctx.timeSigDenominator = 4;
  ReleaseSRWLockExclusive(&load_lock);
  c->v3 = v; c->sr = sr; c->name = strdup(ci.name); return NULL;
fail_all:
  if (v->ctrl) { V3(v->ctrl, v3_controller, setComponentHandler, NULL); if (!v->single) V3(v->ctrl, v3_pluginbase, terminate); V3(v->ctrl, v3_unknown, release); }
  if (v->cp_comp) V3(v->cp_comp, v3_unknown, release);
  if (v->cp_ctrl) V3(v->cp_ctrl, v3_unknown, release);
  free_busbufs(v->ib, v->nib); free_busbufs(v->ob, v->nob);
fail_comp:
  if (v->proc) V3(v->proc, v3_unknown, release);
  V3(v->comp, v3_pluginbase, terminate); V3(v->comp, v3_unknown, release);
fail:
  v3_module_close(lib, f); ReleaseSRWLockExclusive(&load_lock); free(v); return err;
}

static void v3_sync_ctl(conn_t *c) {                                      // [main thread] give the controller the values the app sent
  v3_t *v = V3OF(c); struct { ParamID id; double v; } t[256];
  EnterCriticalSection(&c->qlock); int nt = v->nto_ctl; memcpy(t, v->to_ctl, sizeof(t[0]) * (size_t)nt); v->nto_ctl = 0; LeaveCriticalSection(&c->qlock);
  if (v->ctrl) for (int i = 0; i < nt; i++) V3(v->ctrl, v3_controller, setParamNormalized, t[i].id, t[i].v);
}
static void v3_text(conn_t *c, ParamID id, double value, char *out, size_t cap) {   // [main thread]
  String128 t; memset(t, 0, sizeof t); out[0] = 0;
  if (V3OF(c)->ctrl && V3(V3OF(c)->ctrl, v3_controller, getParamStringByValue, id, value, t) == kResultOk) u16_to_utf8(t, out, cap);
}
static void v3_put_params(conn_t *c, sb_t *s, int full) {                 // [main thread] ,"params":[...]
  v3_t *v = V3OF(c); sb_str(s, ",\"params\":["); int first = 1;
  int32_t n = v->ctrl ? V3(v->ctrl, v3_controller, getParameterCount) : 0;
  for (int32_t i = 0; i < n; i++) {
    ParameterInfo pi; memset(&pi, 0, sizeof pi); if (V3(v->ctrl, v3_controller, getParameterInfo, i, &pi) != kResultOk) continue;
    double val = V3(v->ctrl, v3_controller, getParamNormalized, pi.id); char text[300], units[300]; v3_text(c, pi.id, val, text, sizeof text); u16_to_utf8(pi.units, units, sizeof units);
    if (*units) { size_t k = strlen(text); if (k + strlen(units) + 2 < sizeof text) { text[k] = ' '; copy_str(text + k + 1, units); } }
    sb_fmt(s, "%s{\"id\":%u,\"value\":", first ? "" : ",", (unsigned)pi.id); first = 0; sb_num(s, val); sb_str(s, ",\"text\":"); sb_json(s, text);
    if (full) {
      char name[300]; u16_to_utf8(pi.title, name, sizeof name);
      sb_str(s, ",\"name\":"); sb_json(s, name); sb_str(s, ",\"module\":\"\",\"min\":0,\"max\":1,\"def\":"); sb_num(s, pi.defaultNormalizedValue);
      sb_fmt(s, ",\"stepped\":false,\"hidden\":%s,\"readonly\":%s", pi.flags & kParamIsHidden ? "true" : "false", pi.flags & (kParamIsReadOnly | kParamIsProgramChange) ? "true" : "false");
    }
    sb_str(s, "}");
  }
  sb_str(s, "]");
}

// State: "V3" + 4-byte length + the component's state + 4-byte length + the controller's state.
static int v3_state_save(conn_t *c, sb_t *raw) {                          // [main thread]
  v3_t *v = V3OF(c); v3_stream *a = stream_new(NULL, 0), *b = stream_new(NULL, 0);
  V3(v->comp, v3_component, getState, a); if (v->ctrl && !v->single) V3(v->ctrl, v3_controller, getState, b);
  uint32_t la = (uint32_t)a->n, lb = (uint32_t)b->n; sb_put(raw, "V3", 2); sb_put(raw, &la, 4); if (la) sb_put(raw, a->d, la); sb_put(raw, &lb, 4); if (lb) sb_put(raw, b->d, lb);
  v3st_rel(a); v3st_rel(b); return 1;
}
static int v3_state_load(conn_t *c, const uint8_t *d, size_t n) {         // [main thread]
  v3_t *v = V3OF(c); uint32_t la, lb;
  if (n < 10 || memcmp(d, "V3", 2)) return 0;
  memcpy(&la, d + 2, 4);
  if (6 + (size_t)la + 4 > n) return 0;
  memcpy(&lb, d + 6 + la, 4);
  if (10 + (size_t)la + lb > n) return 0;
  v3_stream *a = stream_new(d + 6, la), *b = stream_new(d + 10 + la, lb);
  AcquireSRWLockExclusive(&load_lock);
  tresult r = V3(v->comp, v3_component, setState, a);
  if (v->ctrl && !v->single) { a->pos = 0; V3(v->ctrl, v3_controller, setComponentState, a); if (lb) V3(v->ctrl, v3_controller, setState, b); }
  ReleaseSRWLockExclusive(&load_lock);
  v3st_rel(a); v3st_rel(b); return r == kResultOk;
}

static const char *v3_gui_show(conn_t *c) {                              // [main thread]
  v3_t *v = V3OF(c); if (!v->ctrl) return "This plug-in has no window of its own";
  if (c->hwnd) { ShowWindow(c->hwnd, 5); return NULL; }
  void *view = V3(v->ctrl, v3_controller, createView, "editor"); if (!view) return "This plug-in has no window of its own";
  if (V3(view, v3_view, isPlatformTypeSupported, "HWND") != kResultTrue) { V3(view, v3_unknown, release); return "The plug-in's window cannot be shown here"; }
  if (!make_window(c)) { V3(view, v3_unknown, release); return "Wine could not create a window"; }
  V3(view, v3_view, setFrame, &v->frame);
  if (V3(view, v3_view, attached, c->hwnd, "HWND") != kResultOk) { V3(view, v3_view, setFrame, NULL); V3(view, v3_unknown, release); drop_window(c); return "The plug-in could not open its window"; }
  v->view = view; ViewRect r = { 0, 0, 0, 0 };
  if (V3(view, v3_view, getSize, &r) == kResultOk && r.right > r.left && r.bottom > r.top) size_window(c, r.right - r.left, r.bottom - r.top);
  ShowWindow(c->hwnd, 5); return NULL;
}
static void v3_gui_close(conn_t *c) {                                     // [main thread]
  v3_t *v = V3OF(c); if (!v->view) return;
  V3(v->view, v3_view, removed); V3(v->view, v3_view, setFrame, NULL); V3(v->view, v3_unknown, release); v->view = NULL;
}

// [audio thread] Run one block: notes and controllers from the MIDI queue become events and mapped parameter changes.
static void v3_process_block(conn_t *c, uint32_t pos, uint32_t n, const float *in, float *o) {
  v3_t *v = V3OF(c);
  struct { int32_t off; ParamID id; double val; } pc[MAXQ * 2]; int npc = 0;
  v->ev.n = 0; v->pin.n = 0; v->pout.n = 0;
  EnterCriticalSection(&c->qlock);
  int keep = 0;
  for (int i = 0; i < c->nmidi; i++) {                                    // notes and controllers due in this block
    int32_t off = c->mtimed[i] ? (int32_t)(c->mat[i] - pos) : 0;
    if (off >= (int32_t)n) { c->midi[keep] = c->midi[i]; c->mat[keep] = c->mat[i]; c->mtimed[keep] = c->mtimed[i]; keep++; continue; }
    if (off < 0) off = 0;
    const uint8_t *m = (const uint8_t *)c->midi[i].midi; int st = m[0] & 0xF0;
    if ((st == 0x90 || st == 0x80) && v->ev.n < 256) {
      V3Event e; memset(&e, 0, sizeof e); e.sampleOffset = off;
      if (st == 0x90 && m[2]) { e.type = kNoteOnEvent; e.noteOn.channel = m[0] & 15; e.noteOn.pitch = m[1] & 127; e.noteOn.velocity = m[2] / 127.0f; e.noteOn.noteId = -1; }
      else { e.type = kNoteOffEvent; e.noteOff.channel = m[0] & 15; e.noteOff.pitch = m[1] & 127; e.noteOff.velocity = 0; e.noteOff.noteId = -1; }
      int k = v->ev.n++; while (k && v->ev.e[k - 1].sampleOffset > off) { v->ev.e[k] = v->ev.e[k - 1]; k--; } v->ev.e[k] = e;
    } else if (npc < MAXQ * 2) {
      int ctl = st == 0xE0 ? kPitchBend : st == 0xD0 ? kAfterTouch : st == 0xB0 ? (m[1] & 127) : -1;
      if (ctl >= 0 && v->midimap[ctl] >= 0) { pc[npc].off = off; pc[npc].id = (ParamID)v->midimap[ctl]; pc[npc].val = st == 0xE0 ? (m[2] << 7 | m[1]) / 16383.0 : (st == 0xD0 ? m[1] : m[2]) / 127.0; npc++; }
    }
  }
  c->nmidi = keep; keep = 0;
  for (int i = 0; i < c->npq; i++) {                                      // parameter changes due in this block
    int32_t off = c->pq[i].timed ? (int32_t)(c->pq[i].at - pos) : 0;
    if (off >= (int32_t)n) { c->pq[keep++] = c->pq[i]; continue; }
    if (npc < MAXQ * 2) { pc[npc].off = off < 0 ? 0 : off; pc[npc].id = (ParamID)c->pq[i].id; pc[npc].val = c->pq[i].v; npc++; }
  }
  c->npq = keep; LeaveCriticalSection(&c->qlock);
  for (int i = 1; i < npc; i++) for (int k = i; k > 0 && pc[k - 1].off > pc[k].off; k--) { typeof(pc[0]) t = pc[k]; pc[k] = pc[k - 1]; pc[k - 1] = t; }
  for (int i = 0; i < npc; i++) { int32_t idx; void *q = v3pc_add(&v->pin, &pc[i].id, &idx); if (q) v3q_add(q, pc[i].off, pc[i].val, NULL); }
  if (v->nib) { AudioBusBuffers *b = &v->ib[0]; for (int k = 0; k < b->numChannels; k++) for (uint32_t i = 0; i < n; i++) b->channelBuffers32[k][i] = in[i * 2 + (k & 1)]; }
  for (int j = 1; j < v->nib; j++) for (int k = 0; k < v->ib[j].numChannels; k++) memset(v->ib[j].channelBuffers32[k], 0, n * 4);
  for (int j = 0; j < v->nob; j++) v->ob[j].silenceFlags = 0;
  v->ctx.projectTimeSamples = v->ctx.continousTimeSamples = (int64_t)pos; v->ctx.projectTimeMusic = (double)pos / v->ctx.sampleRate * 2;   // 120 bpm
  ProcessData *pd = &v->pd; memset(pd, 0, sizeof *pd);
  pd->processMode = 0; pd->symbolicSampleSize = 0; pd->numSamples = (int32_t)n; pd->numInputs = v->nib; pd->numOutputs = v->nob; pd->inputs = v->nib ? v->ib : NULL; pd->outputs = v->nob ? v->ob : NULL;
  pd->inputParameterChanges = &v->pin; pd->outputParameterChanges = &v->pout; pd->inputEvents = &v->ev; pd->processContext = &v->ctx;
  V3(v->proc, v3_processor, process, pd);
  for (int i = 0; i < v->pout.n; i++) if (v->pout.q[i].n) report_change(c, v->pout.q[i].id, v->pout.q[i].val[v->pout.q[i].n - 1]);
  if (!v->nob) { memset(o, 0, (size_t)n * 8); return; }
  AudioBusBuffers *b = &v->ob[0]; const float *L = b->channelBuffers32[0], *R = b->numChannels > 1 ? b->channelBuffers32[1] : L;
  for (uint32_t i = 0; i < n; i++) { float l = L[i], r = R[i]; o[i * 2] = l > -64 && l < 64 ? l : 0; o[i * 2 + 1] = r > -64 && r < 64 ? r : 0; }   // drops NaN, infinity and runaway values
}
