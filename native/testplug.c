// Two tiny CLAP plug-ins used to test the host: "Undae Test Gain" (stereo effect, one Gain parameter, saves its state,
// has a small X11 window with a draggable gain bar)
// and "Undae Test Sine" (instrument: one sine voice per held key). Built into native/bin/undae-test.clap.
#include <clap/clap.h>
#include "xmini.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  clap_plugin_t plugin; const clap_host_t *host; int synth; double gain, sr, phase[128]; float vel[128]; double bend, mod; int pedal; unsigned char held[128];   // held: released while the pedal was down
  Display *d; Window child; GC gc; double shown; int gui_changed; clap_id timer;   // the window, when open
} tp_t;
#define TP(p) ((tp_t *)(p)->plugin_data)

static const char *fx_feat[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO, NULL };
static const char *syn_feat[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_STEREO, NULL };
static const clap_plugin_descriptor_t descs[2] = {
  { CLAP_VERSION_INIT, "app.undaesonicae.test-gain", "Undae Test Gain", "Undae Sonicae", "", "", "", "0.1.0", "Gain, for testing the host", fx_feat },
  { CLAP_VERSION_INIT, "app.undaesonicae.test-sine", "Undae Test Sine", "Undae Sonicae", "", "", "", "0.1.0", "Sine synth, for testing the host", syn_feat },
};

static uint32_t ports_count(const clap_plugin_t *p, bool in) { return in && TP(p)->synth ? 0 : 1; }
static bool ports_get(const clap_plugin_t *p, uint32_t i, bool in, clap_audio_port_info_t *o) {
  (void)p; (void)in; if (i) return false;
  memset(o, 0, sizeof *o); o->id = 0; strcpy(o->name, "Main"); o->flags = CLAP_AUDIO_PORT_IS_MAIN; o->channel_count = 2; o->port_type = CLAP_PORT_STEREO; o->in_place_pair = CLAP_INVALID_ID; return true;
}
static const clap_plugin_audio_ports_t ext_ports = { ports_count, ports_get };

static uint32_t notes_count(const clap_plugin_t *p, bool in) { return in && TP(p)->synth ? 1 : 0; }
static bool notes_get(const clap_plugin_t *p, uint32_t i, bool in, clap_note_port_info_t *o) {
  (void)p; if (i || !in) return false;
  memset(o, 0, sizeof *o); o->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; o->preferred_dialect = CLAP_NOTE_DIALECT_CLAP; strcpy(o->name, "Notes"); return true;
}
static const clap_plugin_note_ports_t ext_notes = { notes_count, notes_get };

static uint32_t par_count(const clap_plugin_t *p) { (void)p; return 1; }
static bool par_info(const clap_plugin_t *p, uint32_t i, clap_param_info_t *o) {
  (void)p; if (i) return false;
  memset(o, 0, sizeof *o); o->id = 1; o->flags = CLAP_PARAM_IS_AUTOMATABLE; strcpy(o->name, "Gain"); o->min_value = 0; o->max_value = 2; o->default_value = 1; return true;
}
static bool par_value(const clap_plugin_t *p, clap_id id, double *v) { if (id != 1) return false; *v = TP(p)->gain; return true; }
static bool par_text(const clap_plugin_t *p, clap_id id, double v, char *b, uint32_t n) { (void)p; if (id != 1) return false; snprintf(b, n, "%.2f x", v); return true; }
static bool par_parse(const clap_plugin_t *p, clap_id id, const char *t, double *v) { (void)p; if (id != 1) return false; *v = atof(t); return true; }
static void key_off(tp_t *t, int k) { if (k < 0 || k > 127) return; if (t->pedal) t->held[k] = 1; else t->vel[k] = 0; }
static void handle(tp_t *t, const clap_event_header_t *e) {
  if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_MIDI) {
    const unsigned char *m = ((const clap_event_midi_t *)e)->data; int st = m[0] & 0xF0;
    if (st == 0x90 && m[2]) { t->vel[m[1] & 127] = m[2] / 127.0f; t->held[m[1] & 127] = 0; }
    else if (st == 0x80 || st == 0x90) key_off(t, m[1]);
    else if (st == 0xE0) t->bend = ((m[2] << 7 | m[1]) - 8192) / 8192.0 * 2;
    else if (st == 0xB0 && m[1] == 1) t->mod = m[2] / 127.0;
    else if (st == 0xB0 && m[1] == 64) { t->pedal = m[2] >= 64; if (!t->pedal) for (int k = 0; k < 128; k++) if (t->held[k]) { t->held[k] = 0; t->vel[k] = 0; } }
    return;
  }
  if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_NOTE_EXPRESSION) { const clap_event_note_expression_t *x = (const void *)e; if (x->expression_id == CLAP_NOTE_EXPRESSION_TUNING) t->bend = x->value; return; }
  if (e->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  if (e->type == CLAP_EVENT_PARAM_VALUE) { const clap_event_param_value_t *v = (const void *)e; if (v->param_id == 1) t->gain = v->value; }
  else if (e->type == CLAP_EVENT_NOTE_ON || e->type == CLAP_EVENT_NOTE_OFF) { const clap_event_note_t *n = (const void *)e; if (e->type == CLAP_EVENT_NOTE_ON) { if (n->key >= 0 && n->key < 128) { t->vel[n->key] = (float)n->velocity; t->held[n->key] = 0; } } else key_off(t, n->key); }
}
static void par_flush(const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *out) { (void)out; for (uint32_t i = 0, n = in->size(in); i < n; i++) handle(TP(p), in->get(in, i)); }
static const clap_plugin_params_t ext_params = { par_count, par_info, par_value, par_text, par_parse, par_flush };

static bool st_save(const clap_plugin_t *p, const clap_ostream_t *s) { return s->write(s, &TP(p)->gain, sizeof(double)) == sizeof(double); }
static bool st_load(const clap_plugin_t *p, const clap_istream_t *s) { double v; if (s->read(s, &v, sizeof v) != sizeof v) return false; TP(p)->gain = v; return true; }
static const clap_plugin_state_t ext_state = { st_save, st_load };

// --- window: a bar showing the gain; click or drag in it to change the gain -------------------------------------
#define GW 320
#define GH 120
static void gui_draw(tp_t *t) {
  char text[64]; int n = snprintf(text, sizeof text, "Gain %.2f x   (click or drag the bar)", t->gain);
  XSetForeground(t->d, t->gc, 0x1e2126); XFillRectangle(t->d, t->child, t->gc, 0, 0, GW, GH);
  XSetForeground(t->d, t->gc, 0x2a2e35); XFillRectangle(t->d, t->child, t->gc, 10, 50, 300, 40);
  XSetForeground(t->d, t->gc, 0x6ea8ff); XFillRectangle(t->d, t->child, t->gc, 10, 50, (unsigned)(300 * t->gain / 2), 40);
  XSetForeground(t->d, t->gc, 0xe7eaef); XDrawString(t->d, t->child, t->gc, 10, 30, text, n);
  XFlush(t->d); t->shown = t->gain;
}
static void gui_pump(tp_t *t) {
  while (t->d && XPending(t->d)) {
    XEvent e; XNextEvent(t->d, &e);
    if (e.type == Expose) gui_draw(t);
    else if (e.type == ButtonPress || e.type == MotionNotify) { double v = (e.xbutton.x - 10) / 300.0; t->gain = (v < 0 ? 0 : v > 1 ? 1 : v) * 2; t->gui_changed = 1; gui_draw(t); }
  }
  if (t->d && t->shown != t->gain) gui_draw(t);                        // the host changed the gain
}
static bool gui_api(const clap_plugin_t *p, const char *api, bool floating) { (void)p; return !strcmp(api, CLAP_WINDOW_API_X11) && !floating; }
static bool gui_pref(const clap_plugin_t *p, const char **api, bool *floating) { (void)p; *api = CLAP_WINDOW_API_X11; *floating = false; return true; }
static bool gui_create(const clap_plugin_t *p, const char *api, bool floating) { return gui_api(p, api, floating); }
static void gui_destroy(const clap_plugin_t *p) {
  tp_t *t = TP(p); if (!t->d) return;
  const clap_host_posix_fd_support_t *hf = t->host->get_extension(t->host, CLAP_EXT_POSIX_FD_SUPPORT); const clap_host_timer_support_t *ht = t->host->get_extension(t->host, CLAP_EXT_TIMER_SUPPORT);
  if (hf) hf->unregister_fd(t->host, XConnectionNumber(t->d));
  if (ht) ht->unregister_timer(t->host, t->timer);
  XFreeGC(t->d, t->gc); XDestroyWindow(t->d, t->child); XCloseDisplay(t->d); t->d = NULL;
}
static bool gui_no_scale(const clap_plugin_t *p, double s) { (void)p; (void)s; return false; }
static bool gui_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { (void)p; *w = GW; *h = GH; return true; }
static bool gui_no(const clap_plugin_t *p) { (void)p; return false; }
static bool gui_no_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h) { (void)p; (void)h; return false; }
static bool gui_no_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h) { (void)p; (void)w; (void)h; return false; }
static bool gui_no_set(const clap_plugin_t *p, uint32_t w, uint32_t h) { (void)p; (void)w; (void)h; return false; }
static bool gui_parent(const clap_plugin_t *p, const clap_window_t *win) {
  tp_t *t = TP(p); t->d = XOpenDisplay(NULL); if (!t->d) return false;
  t->child = XCreateSimpleWindow(t->d, win->x11, 0, 0, GW, GH, 0, 0, 0x1e2126); t->gc = XCreateGC(t->d, t->child, 0, NULL);
  XSelectInput(t->d, t->child, ExposureMask | ButtonPressMask | ButtonMotionMask); XMapWindow(t->d, t->child); XFlush(t->d);
  const clap_host_posix_fd_support_t *hf = t->host->get_extension(t->host, CLAP_EXT_POSIX_FD_SUPPORT); const clap_host_timer_support_t *ht = t->host->get_extension(t->host, CLAP_EXT_TIMER_SUPPORT);
  if (hf) hf->register_fd(t->host, XConnectionNumber(t->d), CLAP_POSIX_FD_READ);
  if (ht) ht->register_timer(t->host, 100, &t->timer);
  return true;
}
static bool gui_no_win(const clap_plugin_t *p, const clap_window_t *w) { (void)p; (void)w; return false; }
static void gui_title(const clap_plugin_t *p, const char *title) { (void)p; (void)title; }
static bool gui_yes(const clap_plugin_t *p) { (void)p; return true; }
static const clap_plugin_gui_t ext_gui = { gui_api, gui_pref, gui_create, gui_destroy, gui_no_scale, gui_size, gui_no, gui_no_hints, gui_no_adjust, gui_no_set, gui_parent, gui_no_win, gui_title, gui_yes, gui_yes };
static void on_fd(const clap_plugin_t *p, int fd, clap_posix_fd_flags_t fl) { (void)fd; (void)fl; gui_pump(TP(p)); }
static const clap_plugin_posix_fd_support_t ext_fd = { on_fd };
static void on_timer(const clap_plugin_t *p, clap_id id) { (void)id; gui_pump(TP(p)); }
static const clap_plugin_timer_support_t ext_timer = { on_timer };

static bool tp_init(const clap_plugin_t *p) { (void)p; return true; }
static void tp_destroy(const clap_plugin_t *p) { gui_destroy(p); free(TP(p)); }
static bool tp_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) { (void)a; (void)b; TP(p)->sr = sr; return true; }
static void tp_deactivate(const clap_plugin_t *p) { (void)p; }
static bool tp_start(const clap_plugin_t *p) { (void)p; return true; }
static void tp_stop(const clap_plugin_t *p) { (void)p; }
static void tp_reset(const clap_plugin_t *p) { memset(TP(p)->vel, 0, sizeof TP(p)->vel); }
static clap_process_status tp_process(const clap_plugin_t *p, const clap_process_t *pr) {
  tp_t *t = TP(p); float *L = pr->audio_outputs[0].data32[0], *R = pr->audio_outputs[0].data32[1];
  uint32_t nev = pr->in_events->size(pr->in_events);
  if (!t->synth) for (uint32_t i = 0; i < nev; i++) handle(t, pr->in_events->get(pr->in_events, i));
  if (t->gui_changed) {                                                // tell the host about a change made in the window
    t->gui_changed = 0; clap_event_param_value_t v = { 0 };
    v.header.size = sizeof v; v.header.space_id = CLAP_CORE_EVENT_SPACE_ID; v.header.type = CLAP_EVENT_PARAM_VALUE; v.param_id = 1; v.note_id = -1; v.port_index = -1; v.channel = -1; v.key = -1; v.value = t->gain;
    pr->out_events->try_push(pr->out_events, &v.header);
  }
  if (!t->synth) { const float *iL = pr->audio_inputs[0].data32[0], *iR = pr->audio_inputs[0].data32[1]; for (uint32_t i = 0; i < pr->frames_count; i++) { L[i] = iL[i] * (float)t->gain; R[i] = iR[i] * (float)t->gain; } return CLAP_PROCESS_CONTINUE; }
  memset(L, 0, pr->frames_count * sizeof(float));
  for (uint32_t from = 0, ei = 0; from < pr->frames_count;) {             // render up to each event's frame, then apply it
    const clap_event_header_t *e = ei < nev ? pr->in_events->get(pr->in_events, ei) : NULL;
    uint32_t to = e && e->time < pr->frames_count ? e->time : pr->frames_count; if (to < from) to = from;
    for (int k = 0; k < 128; k++) if (t->vel[k] > 0) {
      double inc = 440.0 * pow(2, (k - 69 + t->bend) / 12.0) / t->sr, amp = 0.2 * t->vel[k] * t->gain * (1 - 0.5 * t->mod);
      for (uint32_t i = from; i < to; i++) { L[i] += (float)(sin(t->phase[k] * 6.283185307179586) * amp); t->phase[k] += inc; if (t->phase[k] >= 1) t->phase[k] -= 1; }
    }
    from = to; if (e) { handle(t, e); ei++; } else break;
  }
  for (uint32_t ei = 0; ei < nev; ei++) { const clap_event_header_t *e = pr->in_events->get(pr->in_events, ei); if (e->time >= pr->frames_count) handle(t, e); }
  memcpy(R, L, pr->frames_count * sizeof(float));
  return CLAP_PROCESS_CONTINUE;
}
static const void *tp_ext(const clap_plugin_t *p, const char *id) {
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ext_ports;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &ext_params;
  if (!strcmp(id, CLAP_EXT_STATE)) return &ext_state;
  if (!strcmp(id, CLAP_EXT_NOTE_PORTS) && TP(p)->synth) return &ext_notes;
  if (!TP(p)->synth) {
    if (!strcmp(id, CLAP_EXT_GUI)) return &ext_gui;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &ext_fd;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &ext_timer;
  }
  return NULL;
}
static void tp_main(const clap_plugin_t *p) { (void)p; }

static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 2; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i < 2 ? &descs[i] : NULL; }
static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id) {
  (void)f; int which = !strcmp(id, descs[0].id) ? 0 : !strcmp(id, descs[1].id) ? 1 : -1; if (which < 0) return NULL;
  tp_t *t = calloc(1, sizeof *t); t->host = host; t->synth = which; t->gain = 1; t->sr = 48000;
  t->plugin = (clap_plugin_t){ &descs[which], t, tp_init, tp_destroy, tp_activate, tp_deactivate, tp_start, tp_stop, tp_reset, tp_process, tp_ext, tp_main };
  return &t->plugin;
}
static const clap_plugin_factory_t factory = { f_count, f_desc, f_create };
static bool e_init(const char *path) { (void)path; return true; }
static void e_deinit(void) {}
static const void *e_factory(const char *id) { return strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? NULL : &factory; }
CLAP_EXPORT const clap_plugin_entry_t clap_entry = { CLAP_VERSION_INIT, e_init, e_deinit, e_factory };
