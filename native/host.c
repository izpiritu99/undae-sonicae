// Undae Sonicae plug-in host: loads CLAP plug-ins and processes audio for the app over a loopback WebSocket.
// Protocol: native/PROTOCOL.md.  Build: native/build.sh.
//
// Threads: the main thread runs every plug-in "main thread" call (load, parameters, state, unload). Each connection has
// one reader thread, which is that plug-in's audio thread: it processes audio blocks as they arrive and queues control
// messages for the main thread.
#define _GNU_SOURCE
#include <clap/clap.h>
#include "xmini.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXF 4096                 // largest audio block accepted
#define MAXQ 256                  // queued note / parameter events per block
#define MAXMSG (64u << 20)        // largest WebSocket message accepted
#define MAXFD 64                  // file descriptors and timers plug-ins may register

#include "util.h"

// ---------------------------------------------------------------------------------------------- connections
typedef union { clap_event_header_t h; clap_event_note_t note; clap_event_param_value_t pv; clap_event_midi_t midi; clap_event_note_expression_t ne; } ev_t;
typedef struct { ev_t v[MAXQ]; uint32_t n; } evlist_t;
typedef struct { void *handle; const clap_plugin_entry_t *entry; char *path; int refs; } lib_t;

typedef struct conn {
  int fd; pthread_t thread; pthread_mutex_t wlock; atomic_int dead; struct conn *next;
  char *load_js;                                                        // the request that loaded the plug-in, to load it again
  clap_host_t host; lib_t *lib; const clap_plugin_t *plug; void *v3;   // plug: a CLAP plug-in; v3: a VST3 one (vst3lin.h)
  const clap_plugin_params_t *params; const clap_plugin_state_t *state;
  atomic_int active, busy, want_cb, want_rescan; int processing, midi_notes, midi_ok;   // midi_notes: notes must go as MIDI; midi_ok: the plug-in takes MIDI at all
  uint32_t nin, nout; clap_audio_buffer_t *ain, *aout; double sr; int64_t steady;
  pthread_mutex_t qlock; struct { ev_t e; uint32_t at; int timed; } q[MAXQ]; uint32_t qn; evlist_t cur;   // queued events wait for their frame
  uint32_t ncookie; clap_id *cookie_id; void **cookie;
  const clap_plugin_gui_t *gui; const clap_plugin_timer_support_t *tsup; const clap_plugin_posix_fd_support_t *fdsup;
  Window win; int gui_open; uint32_t gw, gh; char *name;
  atomic_int want_out, want_dirty; struct { clap_id id; double value; } out_ev[64]; uint32_t nout_ev;
} conn_t;

typedef struct job { conn_t *c; char *text; struct job *next; } job_t;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER; static int wake_fd;
static void wake(void) { uint64_t one = 1; if (write(wake_fd, &one, 8) < 0) { /* already pending */ } }   // rouse the main thread
static Display *dpy; static Atom wm_delete;
static struct { conn_t *c; int fd; uint32_t flags; void *v3h; } fds[MAXFD]; static int nfds;   // v3h: a VST3 plug-in's handler
static struct { conn_t *c; clap_id id; uint32_t period; int64_t next; void *v3h; } timers[MAXFD]; static int ntimers; static clap_id timer_seq;
static int64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static job_t *jobs, *jobs_tail; static conn_t *conns; static pthread_t main_thread; static char token[80];
static _Thread_local int acting_audio;
static lib_t libs[256]; static int nlibs;
static int serial; static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;

static int write_all(int fd, const void *d, size_t n) { const char *p = d; while (n) { ssize_t k = send(fd, p, n, MSG_NOSIGNAL); if (k <= 0) { if (errno == EINTR) continue; return 0; } p += k; n -= (size_t)k; } return 1; }
static int read_all(int fd, void *d, size_t n) { char *p = d; while (n) { ssize_t k = recv(fd, p, n, 0); if (k <= 0) { if (k < 0 && errno == EINTR) continue; return 0; } p += k; n -= (size_t)k; } return 1; }
static void ws_send(conn_t *c, int op, const void *d, size_t n) {
  uint8_t h[10]; size_t hn = 2; h[0] = 0x80 | op;
  if (n < 126) h[1] = (uint8_t)n; else if (n < 65536) { h[1] = 126; h[2] = n >> 8; h[3] = n & 255; hn = 4; }
  else { h[1] = 127; for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)((uint64_t)n >> (8 * (7 - i))); hn = 10; }
  pthread_mutex_lock(&c->wlock); if (write_all(c->fd, h, hn)) write_all(c->fd, d, n); pthread_mutex_unlock(&c->wlock);
}
static sb_t *capture;                                                  // set: replies are collected here instead of sent (see panic)
static void send_json(conn_t *c, sb_t *s) { if (capture) { sb_put(capture, s->p, s->n); free(s->p); return; } ws_send(c, 1, s->p, s->n); free(s->p); }
static sb_t reply_begin(const char *js, int ok) {
  sb_t s = { 0 }; char *op = jstr(js, "op"); sb_str(&s, "{\"re\":"); sb_json(&s, op); free(op);
  const char *seq = jget(js, "seq"); if (seq) { sb_str(&s, ",\"seq\":"); sb_put(&s, seq, (size_t)(jskip(seq) - seq)); }
  sb_str(&s, ok ? ",\"ok\":true" : ",\"ok\":false"); return s;
}
static void reply_err(conn_t *c, const char *js, const char *msg) { sb_t s = reply_begin(js, 0); sb_str(&s, ",\"error\":"); sb_json(&s, msg); sb_str(&s, "}"); send_json(c, &s); }

// ---------------------------------------------------------------------------------------------- CLAP host side
static const void *host_ext(const clap_host_t *h, const char *id);
static void host_request_restart(const clap_host_t *h) { (void)h; }
static void host_request_process(const clap_host_t *h) { (void)h; }
static void host_request_callback(const clap_host_t *h) { conn_t *c = h->host_data; atomic_store(&c->want_cb, 1); wake(); }
static bool tc_main(const clap_host_t *h) { (void)h; return pthread_equal(pthread_self(), main_thread) && !acting_audio; }
static bool tc_audio(const clap_host_t *h) { (void)h; return !pthread_equal(pthread_self(), main_thread) || acting_audio; }
static const clap_host_thread_check_t ext_thread_check = { tc_main, tc_audio };
static void log_log(const clap_host_t *h, clap_log_severity sev, const char *msg) { (void)h; if (sev >= CLAP_LOG_WARNING) fprintf(stderr, "[plug-in] %s\n", msg); }
static const clap_host_log_t ext_log = { log_log };
static void par_rescan(const clap_host_t *h, clap_param_rescan_flags f) { (void)f; conn_t *c = h->host_data; atomic_store(&c->want_rescan, 1); }
static void par_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags f) { (void)h; (void)id; (void)f; }
static void par_flush(const clap_host_t *h) { (void)h; }
static const clap_host_params_t ext_params = { par_rescan, par_clear, par_flush };
static void state_dirty(const clap_host_t *h) { conn_t *c = h->host_data; atomic_store(&c->want_dirty, 1); wake(); }
static const clap_host_state_t ext_state = { state_dirty };
static void latency_changed(const clap_host_t *h) { (void)h; }
static const clap_host_latency_t ext_latency = { latency_changed };
static bool timer_add(const clap_host_t *h, uint32_t period, clap_id *id) {                // [main thread]
  if (ntimers == MAXFD) return false;
  timers[ntimers].v3h = NULL; timers[ntimers].c = h->host_data; timers[ntimers].id = *id = ++timer_seq; timers[ntimers].period = period < 5 ? 5 : period; timers[ntimers].next = now_ms() + period; ntimers++; return true;
}
static bool timer_del(const clap_host_t *h, clap_id id) { for (int i = 0; i < ntimers; i++) if (timers[i].c == h->host_data && timers[i].id == id) { timers[i] = timers[--ntimers]; return true; } return false; }
static const clap_host_timer_support_t ext_timer = { timer_add, timer_del };
static bool fd_mod(const clap_host_t *h, int fd, clap_posix_fd_flags_t fl) { for (int i = 0; i < nfds; i++) if (fds[i].c == h->host_data && fds[i].fd == fd) { fds[i].flags = fl; return true; } return false; }
static bool fd_add(const clap_host_t *h, int fd, clap_posix_fd_flags_t fl) { if (fd_mod(h, fd, fl)) return true; if (nfds == MAXFD) return false; fds[nfds].v3h = NULL; fds[nfds].c = h->host_data; fds[nfds].fd = fd; fds[nfds].flags = fl; nfds++; return true; }
static bool fd_del(const clap_host_t *h, int fd) { for (int i = 0; i < nfds; i++) if (fds[i].c == h->host_data && fds[i].fd == fd) { fds[i] = fds[--nfds]; return true; } return false; }
static const clap_host_posix_fd_support_t ext_fd = { fd_add, fd_mod, fd_del };
static void gui_hints(const clap_host_t *h) { (void)h; }
static bool gui_resize(const clap_host_t *h, uint32_t w, uint32_t ht) { conn_t *c = h->host_data; if (!c->win || !dpy) return false; c->gw = w; c->gh = ht; XResizeWindow(dpy, c->win, w, ht); XFlush(dpy); return true; }
static bool gui_no(const clap_host_t *h) { (void)h; return false; }
static void gui_closed(const clap_host_t *h, bool destroyed) { (void)h; (void)destroyed; }
static const clap_host_gui_t ext_gui = { gui_hints, gui_resize, gui_no, gui_no, gui_closed };
static const void *host_ext(const clap_host_t *h, const char *id) {
  (void)h;
  if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &ext_timer;
  if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &ext_fd;
  if (!strcmp(id, CLAP_EXT_GUI)) return &ext_gui;
  if (!strcmp(id, CLAP_EXT_THREAD_CHECK)) return &ext_thread_check;
  if (!strcmp(id, CLAP_EXT_LOG)) return &ext_log;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &ext_params;
  if (!strcmp(id, CLAP_EXT_STATE)) return &ext_state;
  if (!strcmp(id, CLAP_EXT_LATENCY)) return &ext_latency;
  return NULL;
}

static lib_t *lib_open(const char *path, char *err, size_t en) {      // [main thread]
  for (int i = 0; i < nlibs; i++) if (libs[i].refs && !strcmp(libs[i].path, path)) { libs[i].refs++; return &libs[i]; }
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL); if (!h) { snprintf(err, en, "Could not open the file: %s", dlerror()); return NULL; }
  const clap_plugin_entry_t *e = dlsym(h, "clap_entry");
  if (!e || !clap_version_is_compatible(e->clap_version)) { snprintf(err, en, "Not a CLAP plug-in this host understands"); dlclose(h); return NULL; }
  if (!e->init(path)) { snprintf(err, en, "The plug-in refused to start"); dlclose(h); return NULL; }
  lib_t *l = NULL; for (int i = 0; i < nlibs; i++) if (!libs[i].refs) { l = &libs[i]; break; }
  if (!l) { if (nlibs == 256) { e->deinit(); dlclose(h); snprintf(err, en, "Too many plug-in files open"); return NULL; } l = &libs[nlibs++]; }
  free(l->path); l->handle = h; l->entry = e; l->path = strdup(path); l->refs = 1; return l;
}
static void lib_close(lib_t *l) { if (l && !--l->refs) { l->entry->deinit(); dlclose(l->handle); } }

static uint32_t ev_size(const clap_input_events_t *l) { return ((evlist_t *)l->ctx)->n; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) { return &((evlist_t *)l->ctx)->v[i].h; }
static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *e) {          // [audio thread] the plug-in reports a change
  conn_t *c = l->ctx; if (e->space_id != CLAP_CORE_EVENT_SPACE_ID || e->type != CLAP_EVENT_PARAM_VALUE) return true;
  const clap_event_param_value_t *v = (const void *)e;
  pthread_mutex_lock(&c->qlock); if (c->nout_ev < 64) { c->out_ev[c->nout_ev].id = v->param_id; c->out_ev[c->nout_ev].value = v->value; c->nout_ev++; } pthread_mutex_unlock(&c->qlock);
  atomic_store(&c->want_out, 1); wake(); return true;
}

static void free_bufs(clap_audio_buffer_t *b, uint32_t n) { if (!b) return; for (uint32_t i = 0; i < n; i++) { for (uint32_t ch = 0; ch < b[i].channel_count; ch++) free(b[i].data32[ch]); free(b[i].data32); } free(b); }

static int x_error(Display *d, void *e) { (void)d; (void)e; return 0; }   // a stale window id must not end the process
#include "vst3lin.h"
static void gui_close(conn_t *c) {                                     // [main thread]
  if (c->v3) { v3_gui_close(c); return; }
  if (!c->gui_open) return;
  c->gui->hide(c->plug); c->gui->destroy(c->plug); c->gui_open = 0;
  if (c->win) { XDestroyWindow(dpy, c->win); XFlush(dpy); c->win = 0; }
}
static const char *gui_show(conn_t *c) {                               // [main thread] NULL on success, else why not
  if (c->v3) return v3_gui_show(c);
  const clap_plugin_t *p = c->plug;
  if (!p || !c->gui) return "This plug-in has no window of its own";
  if (c->gui_open) { if (c->win) { XMapRaised(dpy, c->win); XFlush(dpy); } return NULL; }
  if (!dpy) { dpy = XOpenDisplay(NULL); if (!dpy) return "There is no display to open plug-in windows on"; XSetErrorHandler(x_error); wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", 0); }
  if (c->gui->is_api_supported(p, CLAP_WINDOW_API_X11, false)) {       // the plug-in draws inside a window we own
    if (!c->gui->create(p, CLAP_WINDOW_API_X11, false)) return "The plug-in could not create its window";
    uint32_t w = 640, h = 400; c->gui->set_scale(p, 1.0); c->gui->get_size(p, &w, &h);
    c->win = XCreateSimpleWindow(dpy, XDefaultRootWindow(dpy), 0, 0, w, h, 0, 0, 0);
    XStoreName(dpy, c->win, c->name ? c->name : "Plug-in"); XSetWMProtocols(dpy, c->win, &wm_delete, 1); XSelectInput(dpy, c->win, StructureNotifyMask);
    clap_window_t cw; cw.api = CLAP_WINDOW_API_X11; cw.x11 = c->win;
    if (!c->gui->set_parent(p, &cw)) { c->gui->destroy(p); XDestroyWindow(dpy, c->win); c->win = 0; return "The plug-in could not attach its window"; }
    c->gw = w; c->gh = h; XMapRaised(dpy, c->win); c->gui->show(p); XFlush(dpy);
  } else if (c->gui->is_api_supported(p, CLAP_WINDOW_API_X11, true)) { // the plug-in opens a window itself
    if (!c->gui->create(p, CLAP_WINDOW_API_X11, true)) return "The plug-in could not create its window";
    c->gui->suggest_title(p, c->name ? c->name : "Plug-in"); c->gui->show(p);
  } else return "This plug-in's window needs a display type that is not supported (X11 only)";
  c->gui_open = 1; return NULL;
}

static void plug_unload(conn_t *c) {                                   // [main thread]
  if (c->v3) { v3_unload(c); return; }
  if (!c->plug) return;
  gui_close(c);
  atomic_store(&c->active, 0); while (atomic_load(&c->busy)) usleep(200);
  if (c->processing) { acting_audio = 1; c->plug->stop_processing(c->plug); acting_audio = 0; c->processing = 0; }
  c->plug->deactivate(c->plug); c->plug->destroy(c->plug); c->plug = NULL; c->params = NULL; c->state = NULL;
  lib_close(c->lib); c->lib = NULL; c->gui = NULL; c->tsup = NULL; c->fdsup = NULL; free(c->name); c->name = NULL;
  for (int i = ntimers - 1; i >= 0; i--) if (timers[i].c == c) timers[i] = timers[--ntimers];
  for (int i = nfds - 1; i >= 0; i--) if (fds[i].c == c) fds[i] = fds[--nfds];
  free_bufs(c->ain, c->nin); free_bufs(c->aout, c->nout); c->ain = c->aout = NULL; c->nin = c->nout = 0;
  free(c->cookie_id); free(c->cookie); c->cookie_id = NULL; c->cookie = NULL; c->ncookie = 0;
}

static clap_audio_buffer_t *make_bufs(const clap_plugin_t *p, const clap_plugin_audio_ports_t *ap, bool in, uint32_t *count) {
  uint32_t n = ap ? ap->count(p, in) : 0; *count = n; if (!n) return NULL;
  clap_audio_buffer_t *b = calloc(n, sizeof *b);
  for (uint32_t i = 0; i < n; i++) {
    clap_audio_port_info_t info = { 0 }; ap->get(p, i, in, &info);
    b[i].channel_count = info.channel_count; b[i].data32 = calloc(info.channel_count ? info.channel_count : 1, sizeof(float *));
    for (uint32_t ch = 0; ch < info.channel_count; ch++) b[i].data32[ch] = calloc(MAXF, sizeof(float));
  }
  return b;
}

static void put_params(conn_t *c, sb_t *s, int full) {                 // [main thread] ,"params":[...]
  if (c->v3) { v3_put_params(c, s, full); return; }
  sb_str(s, ",\"params\":[");
  uint32_t n = c->params ? c->params->count(c->plug) : 0; int first = 1;
  for (uint32_t i = 0; i < n; i++) {
    clap_param_info_t pi = { 0 }; if (!c->params->get_info(c->plug, i, &pi)) continue;
    double v = pi.default_value; c->params->get_value(c->plug, pi.id, &v);
    char text[128] = ""; if (!c->params->value_to_text(c->plug, pi.id, v, text, sizeof text)) text[0] = 0;
    sb_fmt(s, "%s{\"id\":%u,\"value\":", first ? "" : ",", pi.id); sb_num(s, v); sb_str(s, ",\"text\":"); sb_json(s, text); first = 0;
    if (full) {
      sb_str(s, ",\"name\":"); sb_json(s, pi.name); sb_str(s, ",\"module\":"); sb_json(s, pi.module);
      sb_str(s, ",\"min\":"); sb_num(s, pi.min_value); sb_str(s, ",\"max\":"); sb_num(s, pi.max_value); sb_str(s, ",\"def\":"); sb_num(s, pi.default_value);
      sb_fmt(s, ",\"stepped\":%s,\"hidden\":%s,\"readonly\":%s", pi.flags & CLAP_PARAM_IS_STEPPED ? "true" : "false", pi.flags & CLAP_PARAM_IS_HIDDEN ? "true" : "false", pi.flags & CLAP_PARAM_IS_READONLY ? "true" : "false");
    }
    sb_str(s, "}");
  }
  sb_str(s, "]");
}

static void op_load(conn_t *c, const char *js) {                       // [main thread]
  if (!capture) { free(c->load_js); c->load_js = strdup(js); }
  char err[400] = "", *path = jstr(js, "path"), *id = jstr(js, "id"); double sr = jnum(js, "sampleRate", 48000);
  plug_unload(c);
  if (path && is_vst3_name(path)) {                                     // VST3
    const char *e3 = v3_load(c, path, id ? id : "", sr); free(path); free(id);
    if (e3) { reply_err(c, js, e3); return; }
    v3_t *v = V3OF(c); int events = V3(v->comp, v3_component, getBusCount, kEvent, kInput) > 0;
    sb_t s = reply_begin(js, 1); sb_str(&s, ",\"id\":\"\",\"name\":"); sb_json(&s, c->name); sb_str(&s, ",\"vendor\":\"\"");
    sb_fmt(&s, ",\"latency\":%u,\"inputs\":%d,\"outputs\":%d,\"notes\":%s,\"state\":true,\"gui\":%s", V3(v->proc, v3_processor, getLatencySamples), v->nib ? v->ib[0].numChannels : 0, v->nob ? v->ob[0].numChannels : 0, events ? "true" : "false", v->ctrl ? "true" : "false");
    put_params(c, &s, 1); sb_str(&s, "}"); send_json(c, &s); atomic_store(&c->active, 1); return;
  }
  lib_t *lib = path ? lib_open(path, err, sizeof err) : NULL;
  if (!lib) { reply_err(c, js, path ? err : "No plug-in file given"); free(path); free(id); return; }
  const clap_plugin_factory_t *f = lib->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  const clap_plugin_descriptor_t *d = NULL;
  for (uint32_t i = 0, n = f ? f->get_plugin_count(f) : 0; i < n && !d; i++) { const clap_plugin_descriptor_t *x = f->get_plugin_descriptor(f, i); if (x && (!id || !*id || !strcmp(x->id, id))) d = x; }
  const clap_plugin_t *p = d ? f->create_plugin(f, &c->host, d->id) : NULL;
  free(path); free(id);
  if (!p) { lib_close(lib); reply_err(c, js, "That plug-in is not in the file"); return; }
  if (!p->init(p)) { p->destroy(p); lib_close(lib); reply_err(c, js, "The plug-in failed to initialise"); return; }
  const clap_plugin_audio_ports_t *ap = p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
  c->ain = make_bufs(p, ap, true, &c->nin); c->aout = make_bufs(p, ap, false, &c->nout);
  if (!p->activate(p, sr, 1, MAXF)) { p->destroy(p); lib_close(lib); free_bufs(c->ain, c->nin); free_bufs(c->aout, c->nout); c->ain = c->aout = NULL; c->nin = c->nout = 0; reply_err(c, js, "The plug-in could not be activated at this sample rate"); return; }
  c->lib = lib; c->plug = p; c->sr = sr; c->steady = 0; c->processing = 0;
  c->params = p->get_extension(p, CLAP_EXT_PARAMS); c->state = p->get_extension(p, CLAP_EXT_STATE);
  c->gui = p->get_extension(p, CLAP_EXT_GUI); c->tsup = p->get_extension(p, CLAP_EXT_TIMER_SUPPORT); c->fdsup = p->get_extension(p, CLAP_EXT_POSIX_FD_SUPPORT); c->name = strdup(d->name);
  int has_gui = c->gui && (c->gui->is_api_supported(p, CLAP_WINDOW_API_X11, false) || c->gui->is_api_supported(p, CLAP_WINDOW_API_X11, true));
  const clap_plugin_note_ports_t *np = p->get_extension(p, CLAP_EXT_NOTE_PORTS); int notes = 0; c->midi_notes = 0; c->midi_ok = 0;
  if (np && np->count(p, true)) { clap_note_port_info_t ni = { 0 }; np->get(p, 0, true, &ni); notes = 1; c->midi_ok = (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0; c->midi_notes = !(ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) && (ni.supported_dialects & CLAP_NOTE_DIALECT_MIDI); }
  c->ncookie = c->params ? c->params->count(p) : 0; c->cookie_id = calloc(c->ncookie + 1, sizeof(clap_id)); c->cookie = calloc(c->ncookie + 1, sizeof(void *));
  for (uint32_t i = 0; i < c->ncookie; i++) { clap_param_info_t pi = { 0 }; if (c->params->get_info(p, i, &pi)) { c->cookie_id[i] = pi.id; c->cookie[i] = pi.cookie; } else c->cookie_id[i] = CLAP_INVALID_ID; }
  const clap_plugin_latency_t *lat = p->get_extension(p, CLAP_EXT_LATENCY);
  sb_t s = reply_begin(js, 1);
  sb_str(&s, ",\"id\":"); sb_json(&s, d->id); sb_str(&s, ",\"name\":"); sb_json(&s, d->name); sb_str(&s, ",\"vendor\":"); sb_json(&s, d->vendor);
  sb_fmt(&s, ",\"latency\":%u,\"inputs\":%u,\"outputs\":%u,\"notes\":%s,\"state\":%s,\"gui\":%s", lat ? lat->get(p) : 0, c->nin ? c->ain[0].channel_count : 0, c->nout ? c->aout[0].channel_count : 0, notes ? "true" : "false", c->state ? "true" : "false", has_gui ? "true" : "false");
  put_params(c, &s, 1); sb_str(&s, "}"); send_json(c, &s);
  atomic_store(&c->active, 1);
}

// `at` is the audio frame (the app's clock, low 32 bits) the event belongs to; untimed events apply at the next block.
static void push_event(conn_t *c, const ev_t *e, const char *js) {
  const char *at = jget(js, "at");
  pthread_mutex_lock(&c->qlock); if (c->qn < MAXQ) { c->q[c->qn].e = *e; c->q[c->qn].timed = at != NULL; c->q[c->qn].at = at ? (uint32_t)(uint64_t)strtod(at, NULL) : 0; c->qn++; } pthread_mutex_unlock(&c->qlock);
}

static void op_param(conn_t *c, const char *js) {
  if (!c->plug && !c->v3) return;
  clap_id id = (clap_id)jnum(js, "id", -1); ev_t e = { 0 };
  e.pv.header.size = sizeof e.pv; e.pv.header.space_id = CLAP_CORE_EVENT_SPACE_ID; e.pv.header.type = CLAP_EVENT_PARAM_VALUE;
  e.pv.param_id = id; e.pv.note_id = -1; e.pv.port_index = -1; e.pv.channel = -1; e.pv.key = -1; e.pv.value = jnum(js, "value", 0);
  for (uint32_t i = 0; i < c->ncookie; i++) if (c->cookie_id[i] == id) e.pv.cookie = c->cookie[i];
  if (c->v3 && !jget(js, "at")) { v3_t *v = V3OF(c); pthread_mutex_lock(&c->qlock); if (v->nto_ctl < 256) { v->to_ctl[v->nto_ctl].id = id; v->to_ctl[v->nto_ctl].v = e.pv.value; v->nto_ctl++; } pthread_mutex_unlock(&c->qlock); wake(); }   // the controller shows it too
  push_event(c, &e, js);
}
// Controllers from the app: pitch bend, mod wheel (CC 1), sustain (CC 64) and other raw MIDI. A plug-in that takes MIDI
// gets them as is. One that only takes CLAP notes gets pitch bend as a tuning expression on all notes (2 semitones); it
// has no equivalent for controllers, so those are dropped (the app holds note-offs itself while the pedal is down).
static int jbytes(const char *js, const char *key, uint8_t *out, int max) {
  const char *p = jget(js, key); int n = 0; if (!p || *p != '[') return 0;
  for (p = jws(p + 1); n < max && (*p == '-' || (*p >= '0' && *p <= '9')); n++) { out[n] = (uint8_t)strtol(p, NULL, 10); p = jws(jskip(p)); if (*p == ',') p = jws(p + 1); }
  return n;
}
static void op_midi(conn_t *c, const char *js) {
  if (!c->plug && !c->v3) return;
  uint8_t d[3] = { 0 }; if (jbytes(js, "data", d, 3) < 1) return; ev_t e = { 0 };
  if (c->midi_ok) {
    e.midi.header.size = sizeof e.midi; e.midi.header.type = CLAP_EVENT_MIDI; e.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    memcpy(e.midi.data, d, 3);
  } else if ((d[0] & 0xF0) == 0xE0) {
    e.ne.header.size = sizeof e.ne; e.ne.header.type = CLAP_EVENT_NOTE_EXPRESSION; e.ne.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.ne.expression_id = CLAP_NOTE_EXPRESSION_TUNING; e.ne.note_id = -1; e.ne.port_index = -1; e.ne.channel = -1; e.ne.key = -1;
    e.ne.value = ((d[2] << 7 | d[1]) - 8192) / 8192.0 * 2;
  } else return;
  push_event(c, &e, js);
}
static void op_note(conn_t *c, const char *js) {
  if (!c->plug && !c->v3) return;
  int on = jtrue(js, "on"), key = (int)jnum(js, "key", 60); double vel = jnum(js, "vel", 0.8); ev_t e = { 0 };
  if (c->midi_notes) {
    e.midi.header.size = sizeof e.midi; e.midi.header.type = CLAP_EVENT_MIDI; e.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.midi.data[0] = on ? 0x90 : 0x80; e.midi.data[1] = (uint8_t)(key & 127); e.midi.data[2] = (uint8_t)(on ? fmax(1, fmin(127, vel * 127)) : 0);
  } else {
    e.note.header.size = sizeof e.note; e.note.header.type = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF; e.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.note.note_id = -1; e.note.port_index = 0; e.note.channel = 0; e.note.key = (int16_t)key; e.note.velocity = vel;
  }
  push_event(c, &e, js);
}

typedef struct { const uint8_t *d; size_t n, at; } rd_t;
static int64_t st_write(const clap_ostream_t *s, const void *b, uint64_t n) { sb_put(s->ctx, b, n); return (int64_t)n; }
static int64_t st_read(const clap_istream_t *s, void *b, uint64_t n) { rd_t *r = s->ctx; uint64_t k = r->n - r->at < n ? r->n - r->at : n; memcpy(b, r->d + r->at, k); r->at += k; return (int64_t)k; }
static void op_state(conn_t *c, const char *js) {                      // [main thread]
  if (c->v3) {
    char *data = jstr(js, "data");
    if (data) {
      size_t n = strlen(data); uint8_t *raw = malloc(n + 1); size_t len = b64_dec(data, n, raw); free(data);
      int ok = v3_state_load(c, raw, len); free(raw); if (!ok) { reply_err(c, js, "The saved state does not fit this plug-in"); return; }
      sb_t s = reply_begin(js, 1); put_params(c, &s, 0); sb_str(&s, "}"); send_json(c, &s);
    } else { sb_t raw = { 0 }; v3_state_save(c, &raw); sb_t s = reply_begin(js, 1); sb_str(&s, ",\"data\":\""); b64_enc(&s, (const uint8_t *)raw.p, raw.n); sb_str(&s, "\"}"); free(raw.p); send_json(c, &s); }
    return;
  }
  if (!c->plug || !c->state) { reply_err(c, js, "This plug-in does not save its state"); return; }
  char *data = jstr(js, "data");
  if (data) {
    size_t n = strlen(data); uint8_t *raw = malloc(n + 1); rd_t r = { raw, b64_dec(data, n, raw), 0 }; clap_istream_t in = { &r, st_read };
    bool ok = c->state->load(c->plug, &in); free(raw); free(data);
    if (!ok) { reply_err(c, js, "The plug-in rejected the saved state"); return; }
    sb_t s = reply_begin(js, 1); put_params(c, &s, 0); sb_str(&s, "}"); send_json(c, &s);
  } else {
    sb_t raw = { 0 }; clap_ostream_t out = { &raw, st_write };
    if (!c->state->save(c->plug, &out)) { free(raw.p); reply_err(c, js, "The plug-in could not save its state"); return; }
    sb_t s = reply_begin(js, 1); sb_str(&s, ",\"data\":\""); b64_enc(&s, (const uint8_t *)raw.p, raw.n); sb_str(&s, "\"}"); free(raw.p); send_json(c, &s);
  }
}

static void scan_file(const char *path, sb_t *s, int *first) {         // [main thread]
  char err[200]; lib_t *l = lib_open(path, err, sizeof err); if (!l) return;
  const clap_plugin_factory_t *f = l->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  for (uint32_t i = 0, n = f ? f->get_plugin_count(f) : 0; i < n; i++) {
    const clap_plugin_descriptor_t *d = f->get_plugin_descriptor(f, i); if (!d) continue;
    int inst = 0; for (const char *const *ft = d->features; ft && *ft; ft++) if (!strcmp(*ft, CLAP_PLUGIN_FEATURE_INSTRUMENT)) inst = 1;
    sb_str(s, *first ? "{\"id\":" : ",{\"id\":"); *first = 0; sb_json(s, d->id); sb_str(s, ",\"name\":"); sb_json(s, d->name); sb_str(s, ",\"vendor\":"); sb_json(s, d->vendor);
    sb_str(s, ",\"version\":"); sb_json(s, d->version); sb_str(s, ",\"path\":"); sb_json(s, path); sb_fmt(s, ",\"format\":\"clap\",\"instrument\":%s}", inst ? "true" : "false");
  }
  lib_close(l);
}
static void scan_dir(const char *dir, int depth, sb_t *s, int *first) {
  DIR *d = opendir(dir); if (!d) return;
  for (struct dirent *e; (e = readdir(d));) {
    if (e->d_name[0] == '.') continue;
    char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name); struct stat st; if (stat(p, &st)) continue;
    size_t n = strlen(e->d_name);
    if (is_vst3_name(e->d_name)) v3_scan(p, s, first);                 // a .vst3 bundle (or file)
    else if (S_ISDIR(st.st_mode)) { if (depth < 4) scan_dir(p, depth + 1, s, first); }
    else if (n > 5 && !strcmp(e->d_name + n - 5, ".clap")) scan_file(p, s, first);
  }
  closedir(d);
}
static void op_scan(conn_t *c, const char *js) {
  sb_t s = reply_begin(js, 1); int first = 1; sb_str(&s, ",\"plugins\":[");
  const char *a = jget(js, "paths");
  if (a && *a == '[') for (const char *p = jws(a + 1); *p == '"'; ) { char *dir = jstr_at(p); if (dir) { scan_dir(dir, 0, &s, &first); free(dir); } p = jws(jskip(p)); if (*p == ',') p = jws(p + 1); }
  {
    const char *home = getenv("HOME"), *env = getenv("CLAP_PATH"); char p[4096];
    if (home) { snprintf(p, sizeof p, "%s/.clap", home); scan_dir(p, 0, &s, &first); }
    scan_dir("/usr/lib/clap", 0, &s, &first); scan_dir("/usr/local/lib/clap", 0, &s, &first);
    if (home) { snprintf(p, sizeof p, "%s/.vst3", home); scan_dir(p, 0, &s, &first); }
    scan_dir("/usr/lib/vst3", 0, &s, &first); scan_dir("/usr/local/lib/vst3", 0, &s, &first);
    if (env) { char *dup = strdup(env); for (char *t = strtok(dup, ":"); t; t = strtok(NULL, ":")) scan_dir(t, 0, &s, &first); free(dup); }
  }
  sb_str(&s, "]}"); send_json(c, &s);
}

static void run_job(conn_t *c, const char *js) {                       // [main thread]
  char *op = jstr(js, "op"); if (!op) return;
  if (c->v3) v3_sync_ctl(c);
  if (!strcmp(op, "hello")) { sb_t s = reply_begin(js, 1); sb_str(&s, ",\"host\":\"undae-host\",\"version\":1,\"formats\":[\"clap\",\"vst3\"],\"gui\":true}"); send_json(c, &s); }
  else if (!strcmp(op, "scan")) op_scan(c, js);
  else if (!strcmp(op, "load")) op_load(c, js);
  else if (!strcmp(op, "panic")) {                                    // transport stopped: silence an instrument now, release tails included
    if (c->plug || c->v3) {
      atomic_store(&c->active, 0); while (atomic_load(&c->busy)) usleep(100);
      pthread_mutex_lock(&c->qlock); c->qn = 0; pthread_mutex_unlock(&c->qlock);
      if (c->plug) { acting_audio = 1; c->plug->reset(c->plug); acting_audio = 0; atomic_store(&c->active, 1); }
      else if (c->load_js) {                                          // VST3 instruments keep sounding through a reset: replace the instance, keep its state
        sb_t got = { 0 }; int had_window = c->gui_open;
        capture = &got; op_state(c, "{\"op\":\"state\"}"); capture = NULL;
        char *data = got.p ? jstr(got.p, "data") : NULL; free(got.p);
        char *again = strdup(c->load_js); got = (sb_t){ 0 }; capture = &got; op_load(c, again); free(again);
        if (data && c->v3) { sb_t js2 = { 0 }; sb_str(&js2, "{\"op\":\"state\",\"data\":"); sb_json(&js2, data); sb_str(&js2, "}"); op_state(c, js2.p); free(js2.p); }
        capture = NULL; free(got.p); free(data);
        pthread_mutex_lock(&c->qlock); c->qn = 0; pthread_mutex_unlock(&c->qlock);
        if (had_window && c->v3) gui_show(c);
      }
    }
  }
  else if (!strcmp(op, "unload")) { plug_unload(c); sb_t s = reply_begin(js, 1); sb_str(&s, "}"); send_json(c, &s); }
  else if (!strcmp(op, "params")) { if (c->plug || c->v3) { sb_t s = reply_begin(js, 1); put_params(c, &s, 0); sb_str(&s, "}"); send_json(c, &s); } else reply_err(c, js, "No plug-in loaded"); }
  else if (!strcmp(op, "state")) op_state(c, js);
  else if (!strcmp(op, "gui")) {
    const char *err = NULL; if (jtrue(js, "show")) err = gui_show(c); else gui_close(c);
    if (err) reply_err(c, js, err); else { sb_t s = reply_begin(js, 1); sb_fmt(&s, ",\"open\":%s}", c->gui_open ? "true" : "false"); send_json(c, &s); }
  }
  else reply_err(c, js, "Unknown request");
  free(op);
}

// ---------------------------------------------------------------------------------------------- audio thread
static void process_block(conn_t *c, const uint8_t *msg, size_t len) {
  uint32_t head[3]; memcpy(head, msg, 12); uint32_t n = head[2];
  if (head[0] != 1 || !n || n > MAXF || len < 12 + (size_t)n * 8) return;
  static _Thread_local float out[12 / 4 + MAXF * 2];
  const float *in = (const float *)(msg + 12); float *o = out + 3; memcpy(out, head, 12);
  atomic_store(&c->busy, 1);
  if (!atomic_load(&c->active)) { atomic_store(&c->busy, 0); memcpy(o, in, (size_t)n * 8); ws_send(c, 2, out, 12 + (size_t)n * 8); return; }
  if (c->v3) { v3_process_block(c, head[1], n, in, o); atomic_store(&c->busy, 0); ws_send(c, 2, out, 12 + (size_t)n * 8); return; }
  const clap_plugin_t *p = c->plug;
  if (!c->processing) c->processing = p->start_processing(p);
  c->cur.n = 0;
  if (!pthread_mutex_trylock(&c->qlock)) {                              // take the events that fall inside this block
    uint32_t keep = 0, pos = head[1];
    for (uint32_t i = 0; i < c->qn; i++) {
      int32_t off = c->q[i].timed ? (int32_t)(c->q[i].at - pos) : 0;
      if (off >= (int32_t)n) { c->q[keep++] = c->q[i]; continue; }
      ev_t e = c->q[i].e; e.h.time = off < 0 ? 0 : (uint32_t)off;
      uint32_t k = c->cur.n++; while (k && c->cur.v[k - 1].h.time > e.h.time) { c->cur.v[k] = c->cur.v[k - 1]; k--; }   // keep them in time order
      c->cur.v[k] = e;
    }
    c->qn = keep; pthread_mutex_unlock(&c->qlock);
  }
  if (c->nin) {
    clap_audio_buffer_t *b = &c->ain[0];
    if (b->channel_count >= 2) for (uint32_t i = 0; i < n; i++) { b->data32[0][i] = in[i * 2]; b->data32[1][i] = in[i * 2 + 1]; }
    else if (b->channel_count == 1) for (uint32_t i = 0; i < n; i++) b->data32[0][i] = (in[i * 2] + in[i * 2 + 1]) * 0.5f;
  }
  clap_input_events_t ie = { &c->cur, ev_size, ev_get }; clap_output_events_t oe = { c, ev_push };
  clap_process_t pr = { 0 }; pr.steady_time = c->steady; pr.frames_count = n; pr.audio_inputs = c->ain; pr.audio_outputs = c->aout; pr.audio_inputs_count = c->nin; pr.audio_outputs_count = c->nout; pr.in_events = &ie; pr.out_events = &oe;
  clap_process_status st = c->processing ? p->process(p, &pr) : CLAP_PROCESS_ERROR; c->steady += n;
  atomic_store(&c->busy, 0);
  clap_audio_buffer_t *b = c->nout ? &c->aout[0] : NULL;
  if (st == CLAP_PROCESS_ERROR || !b || !b->channel_count) memset(o, 0, (size_t)n * 8);
  else { const float *L = b->data32[0], *R = b->channel_count > 1 ? b->data32[1] : L; for (uint32_t i = 0; i < n; i++) { float l = L[i], r = R[i]; o[i * 2] = l > -64 && l < 64 ? l : 0; o[i * 2 + 1] = r > -64 && r < 64 ? r : 0; } }   // drops NaN, infinity and runaway values
  ws_send(c, 2, out, 12 + (size_t)n * 8);
}

static int handshake(int fd) {
  char req[8192]; size_t n = 0;
  while (n < sizeof req - 1) { ssize_t k = recv(fd, req + n, sizeof req - 1 - n, 0); if (k <= 0) return 0; n += (size_t)k; req[n] = 0; if (strstr(req, "\r\n\r\n")) break; }
  sb_t s = { 0 }; if (!ws_accept(req, token, &s)) { write_all(fd, "HTTP/1.1 403 Forbidden\r\n\r\n", 26); return 0; }
  int ok = write_all(fd, s.p, s.n); free(s.p); return ok;
}

static void *reader(void *arg) {
  conn_t *c = arg; sb_t msg = { 0 }; int msg_op = 0;
  if (handshake(c->fd)) for (;;) {
    uint8_t h[2], ext[8], mask[4]; if (!read_all(c->fd, h, 2)) break;
    int fin = h[0] & 0x80, op = h[0] & 15; uint64_t n = h[1] & 127;
    if (n == 126) { if (!read_all(c->fd, ext, 2)) break; n = (uint64_t)ext[0] << 8 | ext[1]; }
    else if (n == 127) { if (!read_all(c->fd, ext, 8)) break; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | ext[i]; }
    if (!(h[1] & 0x80) || n > MAXMSG || msg.n + n > MAXMSG) break;                  // clients must mask
    if (!read_all(c->fd, mask, 4)) break;
    size_t at = msg.n; if (msg.cap < at + n + 1) { msg.cap = (at + n + 1) * 2; msg.p = realloc(msg.p, msg.cap); }
    if (n && !read_all(c->fd, msg.p + at, n)) break;
    for (uint64_t i = 0; i < n; i++) msg.p[at + i] ^= mask[i & 3];
    if (op == 8) break;
    if (op == 9) { ws_send(c, 10, msg.p + at, n); continue; }
    if (op == 10) continue;
    if (op) msg_op = op;
    msg.n = at + n; msg.p[msg.n] = 0;
    if (!fin) continue;
    if (msg_op == 2) {                                                  // UNDAE_SERIAL=1: one plug-in processes at a time (multi-threaded processing off)
      if (serial) pthread_mutex_lock(&serial_lock);
      process_block(c, (const uint8_t *)msg.p, msg.n);
      if (serial) pthread_mutex_unlock(&serial_lock);
    }
    else if (msg_op == 1) {
      char *op_name = jstr(msg.p, "op");
      if (op_name && (!strcmp(op_name, "param") || !strcmp(op_name, "note") || !strcmp(op_name, "midi"))) {   // cheap and frequent: straight to the event queue
        atomic_store(&c->busy, 1); if (atomic_load(&c->active)) (op_name[0] == 'p' ? op_param : op_name[0] == 'n' ? op_note : op_midi)(c, msg.p); atomic_store(&c->busy, 0);
      }
      else { job_t *j = calloc(1, sizeof *j); j->c = c; j->text = strdup(msg.p); pthread_mutex_lock(&mlock); if (jobs_tail) jobs_tail->next = j; else jobs = j; jobs_tail = j; pthread_mutex_unlock(&mlock); wake(); }
      free(op_name);
    }
    msg.n = 0;
  }
  free(msg.p); shutdown(c->fd, SHUT_RDWR);
  atomic_store(&c->dead, 1); wake();
  return NULL;
}

static void *acceptor(void *arg) {
  int ls = (int)(intptr_t)arg;
  for (;;) {
    int fd = accept(ls, NULL, NULL); if (fd < 0) { if (errno == EINTR) continue; break; }
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    conn_t *c = calloc(1, sizeof *c); c->fd = fd; pthread_mutex_init(&c->wlock, NULL); pthread_mutex_init(&c->qlock, NULL);
    c->host = (clap_host_t){ CLAP_VERSION_INIT, c, "Undae Sonicae", "Undae Sonicae", "", "0.1.0", host_ext, host_request_restart, host_request_process, host_request_callback };
    pthread_mutex_lock(&mlock); c->next = conns; conns = c; pthread_mutex_unlock(&mlock);
    pthread_create(&c->thread, NULL, reader, c);
  }
  return NULL;
}

static conn_t *conn_by_window(Window w) { pthread_mutex_lock(&mlock); conn_t *c = conns; while (c && c->win != w) c = c->next; pthread_mutex_unlock(&mlock); return c; }
static void x_events(void) {                                           // [main thread]
  while (dpy && XPending(dpy)) {
    XEvent e; XNextEvent(dpy, &e);
    if (e.type == ClientMessage && (Atom)e.xclient.l[0] == wm_delete) {  // the user closed the window
      conn_t *c = conn_by_window(e.xclient.window); if (!c) continue;
      gui_close(c); sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"gui\",\"open\":false}"); send_json(c, &s);
    } else if (e.type == ConfigureNotify) {
      conn_t *c = conn_by_window(e.xconfigure.window); uint32_t w = (uint32_t)e.xconfigure.width, h = (uint32_t)e.xconfigure.height;
      if (c && c->v3) v3_gui_resized(c, w, h);
      else if (c && c->gui_open && (w != c->gw || h != c->gh)) {
        if (c->gui->can_resize(c->plug)) { c->gui->adjust_size(c->plug, &w, &h); c->gui->set_size(c->plug, w, h); c->gw = w; c->gh = h; }
        else { XResizeWindow(dpy, c->win, c->gw, c->gh); XFlush(dpy); }
      }
    }
  }
}
static int registered_fd(conn_t *c, int fd) { for (int i = 0; i < nfds; i++) if (fds[i].c == c && fds[i].fd == fd) return 1; return 0; }
static int registered_timer(conn_t *c, clap_id id) { for (int i = 0; i < ntimers; i++) if (timers[i].c == c && timers[i].id == id) return 1; return 0; }
static void wait_events(void) {                                        // [main thread] sleep until something needs it, then run plug-in fds and timers
  struct pollfd pf[2 + MAXFD]; int n = 0, base; int64_t now = now_ms(), to = 50;
  pf[n++] = (struct pollfd){ wake_fd, POLLIN, 0 };
  if (dpy) pf[n++] = (struct pollfd){ XConnectionNumber(dpy), POLLIN, 0 };
  base = n;
  for (int i = 0; i < nfds; i++) pf[n++] = (struct pollfd){ fds[i].fd, (short)((fds[i].flags & CLAP_POSIX_FD_READ ? POLLIN : 0) | (fds[i].flags & CLAP_POSIX_FD_WRITE ? POLLOUT : 0)), 0 };
  for (int i = 0; i < ntimers; i++) if (timers[i].next - now < to) to = timers[i].next > now ? timers[i].next - now : 0;
  if (dpy && XPending(dpy)) to = 0;
  poll(pf, (nfds_t)n, (int)to);
  if (pf[0].revents) { uint64_t v; if (read(wake_fd, &v, 8) < 0) { /* nothing pending */ } }
  struct { conn_t *c; int fd; uint32_t fl; void *v3h; } hit[MAXFD]; int nh = 0;   // callbacks may add or remove entries, so work from copies
  for (int i = 0; i < n - base; i++) if (pf[base + i].revents) { hit[nh].c = fds[i].c; hit[nh].v3h = fds[i].v3h; hit[nh].fd = fds[i].fd; hit[nh].fl = (pf[base + i].revents & POLLIN ? CLAP_POSIX_FD_READ : 0) | (pf[base + i].revents & POLLOUT ? CLAP_POSIX_FD_WRITE : 0) | (pf[base + i].revents & (POLLERR | POLLHUP) ? CLAP_POSIX_FD_ERROR : 0); nh++; }
  for (int i = 0; i < nh; i++) if (registered_fd(hit[i].c, hit[i].fd)) { if (hit[i].v3h) V3(hit[i].v3h, v3_eventhandler, onFDIsSet, hit[i].fd); else if (hit[i].c->fdsup) hit[i].c->fdsup->on_fd(hit[i].c->plug, hit[i].fd, hit[i].fl); }
  struct { conn_t *c; clap_id id; void *v3h; } due[MAXFD]; int nd = 0; now = now_ms();
  for (int i = 0; i < ntimers; i++) if (now >= timers[i].next) { timers[i].next = now + timers[i].period; due[nd].c = timers[i].c; due[nd].v3h = timers[i].v3h; due[nd].id = timers[i].id; nd++; }
  for (int i = 0; i < nd; i++) if (registered_timer(due[i].c, due[i].id)) { if (due[i].v3h) V3(due[i].v3h, v3_timerhandler, onTimer); else if (due[i].c->tsup) due[i].c->tsup->on_timer(due[i].c->plug, due[i].id); }
  x_events();
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN); main_thread = pthread_self(); serial = getenv("UNDAE_SERIAL") && *getenv("UNDAE_SERIAL") == '1'; wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); XInitThreads();
  for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--child")) prctl(PR_SET_PDEATHSIG, SIGTERM);   // exit with the app that started us
  const char *env = getenv("UNDAE_HOST_TOKEN");
  if (env && *env && strlen(env) < sizeof token) strcpy(token, env);
  else { uint8_t r[16]; FILE *f = fopen("/dev/urandom", "rb"); if (!f || fread(r, 1, 16, f) != 16) { fprintf(stderr, "no random source\n"); return 1; } fclose(f); for (int i = 0; i < 16; i++) sprintf(token + i * 2, "%02x", r[i]); }
  int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(getenv("UNDAE_HOST_PORT") ? (uint16_t)atoi(getenv("UNDAE_HOST_PORT")) : 0);
  socklen_t al = sizeof a;
  if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16) || getsockname(ls, (struct sockaddr *)&a, &al)) { perror("listen"); return 1; }
  printf("{\"port\":%d,\"token\":\"%s\"}\n", ntohs(a.sin_port), token); fflush(stdout);
  pthread_t at; pthread_create(&at, NULL, acceptor, (void *)(intptr_t)ls);

  pthread_mutex_lock(&mlock);
  for (;;) {
    while (jobs) {
      job_t *j = jobs; jobs = j->next; if (!jobs) jobs_tail = NULL;
      pthread_mutex_unlock(&mlock); if (!atomic_load(&j->c->dead)) run_job(j->c, j->text); free(j->text); free(j); pthread_mutex_lock(&mlock);
    }
    for (conn_t **pc = &conns; *pc && !jobs;) {                         // new requests first: they may refer to these connections
      conn_t *c = *pc;
      if (atomic_load(&c->dead)) {                                    // the queue is empty here, so nothing refers to it any more
        *pc = c->next; pthread_mutex_unlock(&mlock);
        pthread_join(c->thread, NULL); plug_unload(c); close(c->fd); free(c->load_js); free(c);
        pthread_mutex_lock(&mlock); pc = &conns; continue;
      }
      if (c->plug && atomic_exchange(&c->want_cb, 0)) { pthread_mutex_unlock(&mlock); c->plug->on_main_thread(c->plug); pthread_mutex_lock(&mlock); pc = &conns; continue; }
      if (c->v3 && V3OF(c)->nto_ctl) { pthread_mutex_unlock(&mlock); v3_sync_ctl(c); pthread_mutex_lock(&mlock); pc = &conns; continue; }
      if ((c->plug || c->v3) && atomic_exchange(&c->want_out, 0)) {             // changes made in the plug-in's window go back to the app
        pthread_mutex_unlock(&mlock);
        struct { clap_id id; double value; } ev[64]; pthread_mutex_lock(&c->qlock); uint32_t ne = c->nout_ev; memcpy(ev, c->out_ev, sizeof ev); c->nout_ev = 0; pthread_mutex_unlock(&c->qlock);
        sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"param\",\"changes\":[");
        for (uint32_t i = 0; i < ne; i++) { char text[300] = ""; if (c->v3) v3_text(c, ev[i].id, ev[i].value, text, sizeof text); else if (!c->params || !c->params->value_to_text(c->plug, ev[i].id, ev[i].value, text, sizeof text)) text[0] = 0; sb_fmt(&s, "%s{\"id\":%u,\"value\":", i ? "," : "", ev[i].id); sb_num(&s, ev[i].value); sb_str(&s, ",\"text\":"); sb_json(&s, text); sb_str(&s, "}"); }
        sb_str(&s, "]}"); send_json(c, &s); pthread_mutex_lock(&mlock); pc = &conns; continue;
      }
      if ((c->plug || c->v3) && atomic_exchange(&c->want_dirty, 0)) { pthread_mutex_unlock(&mlock); sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"dirty\"}"); send_json(c, &s); pthread_mutex_lock(&mlock); pc = &conns; continue; }
      if ((c->plug || c->v3) && atomic_exchange(&c->want_rescan, 0)) { pthread_mutex_unlock(&mlock); sb_t s = { 0 }; sb_str(&s, "{\"ev\":\"params\""); put_params(c, &s, 1); sb_str(&s, "}"); send_json(c, &s); pthread_mutex_lock(&mlock); pc = &conns; continue; }
      pc = &c->next;
    }
    if (!jobs) { pthread_mutex_unlock(&mlock); wait_events(); pthread_mutex_lock(&mlock); }
  }
}
