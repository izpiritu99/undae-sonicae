// VST3 host objects shared by both plug-in hosts: memory streams, messages and attribute lists, the host application,
// parameter queues and event lists, and string / class-id helpers. Include after ../vst3c.h (or vst3c.h), with
// copy_str(dst, src) defined.
#pragma once

static int same_iid(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 16); }
static void u16_to_utf8(const char16 *s, char *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; s[i] && o + 4 < cap; i++) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++; }
    if (c < 0x80) out[o++] = (char)c;
    else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 63)); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
  }
  out[o] = 0;
}
static void hex_cid(const uint8_t *cid, char *out) { static const char H[] = "0123456789ABCDEF"; for (int i = 0; i < 16; i++) { out[i * 2] = H[cid[i] >> 4]; out[i * 2 + 1] = H[cid[i] & 15]; } out[32] = 0; }
static int parse_cid(const char *hex, uint8_t *cid) {
  if (!hex || strlen(hex) != 32) return 0;
  for (int i = 0; i < 32; i++) { char ch = hex[i]; int v = ch >= '0' && ch <= '9' ? ch - '0' : (ch | 32) >= 'a' && (ch | 32) <= 'f' ? (ch | 32) - 'a' + 10 : -1; if (v < 0) return 0; if (i & 1) cid[i / 2] |= (uint8_t)v; else cid[i / 2] = (uint8_t)(v << 4); }
  return 1;
}

// ---------------------------------------------------------------------------------------------- host objects
static uint32_t PAPI ref_one(void *self) { (void)self; return 1; }   // objects the host owns for the plug-in's lifetime

// A growable memory stream (IBStream), for plug-in state.
typedef struct { const struct v3_stream_vt *vt; int32_t refs; uint8_t *d; int64_t n, cap, pos; } v3_stream;
static tresult PAPI v3st_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IBStream)) { ((v3_stream *)self)->refs++; *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static uint32_t PAPI v3st_add(void *self) { return (uint32_t)++((v3_stream *)self)->refs; }
static uint32_t PAPI v3st_rel(void *self) { v3_stream *s = self; if (--s->refs) return (uint32_t)s->refs; free(s->d); free(s); return 0; }
static tresult PAPI v3st_read(void *self, void *buf, int32_t n, int32_t *got) {
  v3_stream *s = self; int64_t k = s->n - s->pos < n ? s->n - s->pos : n; if (k < 0) k = 0;
  memcpy(buf, s->d + s->pos, (size_t)k); s->pos += k; if (got) *got = (int32_t)k; return kResultOk;
}
static tresult PAPI v3st_write(void *self, void *buf, int32_t n, int32_t *put) {
  v3_stream *s = self; if (n < 0) return kInvalidArgument;
  if (s->pos + n > s->cap) { s->cap = (s->pos + n) * 2 + 256; s->d = realloc(s->d, (size_t)s->cap); }
  memcpy(s->d + s->pos, buf, (size_t)n); s->pos += n; if (s->pos > s->n) s->n = s->pos; if (put) *put = n; return kResultOk;
}
static tresult PAPI v3st_seek(void *self, int64_t pos, int32_t mode, int64_t *res) {
  v3_stream *s = self; int64_t p = mode == 0 ? pos : mode == 1 ? s->pos + pos : s->n + pos;
  if (p < 0) p = 0;
  if (p > s->n) p = s->n;
  s->pos = p; if (res) *res = p; return kResultOk;
}
static tresult PAPI v3st_tell(void *self, int64_t *pos) { *pos = ((v3_stream *)self)->pos; return kResultOk; }
static const struct v3_stream_vt v3st_vt = { v3st_qi, v3st_add, v3st_rel, v3st_read, v3st_write, v3st_seek, v3st_tell };
static v3_stream *stream_new(const void *d, size_t n) {
  v3_stream *s = calloc(1, sizeof *s); s->vt = &v3st_vt; s->refs = 1;
  if (n) { s->d = malloc(n); memcpy(s->d, d, n); s->n = s->cap = (int64_t)n; }
  return s;
}

// Messages and attribute lists, which a component and its controller may ask the host for to talk to each other.
typedef struct { char id[64]; int kind; int64_t i; double f; uint8_t *bin; uint32_t len; } v3_attr;
typedef struct { const struct v3_attrs_vt *vt; int32_t refs; int n; v3_attr a[32]; } v3_attrs;
typedef struct { const struct v3_message_vt *vt; int32_t refs; char id[128]; v3_attrs *attrs; } v3_message;
static v3_attr *attr_slot(v3_attrs *l, const char *id, int make) {
  for (int i = 0; i < l->n; i++) if (!strcmp(l->a[i].id, id)) return &l->a[i];
  if (!make || l->n == 32 || strlen(id) >= 64) return NULL;
  v3_attr *a = &l->a[l->n++]; memset(a, 0, sizeof *a); copy_str(a->id, id); return a;
}
static tresult PAPI v3at_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IAttributeList)) { ((v3_attrs *)self)->refs++; *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static uint32_t PAPI v3at_add(void *self) { return (uint32_t)++((v3_attrs *)self)->refs; }
static uint32_t PAPI v3at_rel(void *self) { v3_attrs *l = self; if (--l->refs) return (uint32_t)l->refs; for (int i = 0; i < l->n; i++) free(l->a[i].bin); free(l); return 0; }
static tresult PAPI v3at_seti(void *self, const char *id, int64_t v) { v3_attr *a = attr_slot(self, id, 1); if (!a) return kInvalidArgument; a->kind = 1; a->i = v; return kResultOk; }
static tresult PAPI v3at_geti(void *self, const char *id, int64_t *v) { v3_attr *a = attr_slot(self, id, 0); if (!a || a->kind != 1) return kResultFalse; *v = a->i; return kResultOk; }
static tresult PAPI v3at_setf(void *self, const char *id, double v) { v3_attr *a = attr_slot(self, id, 1); if (!a) return kInvalidArgument; a->kind = 2; a->f = v; return kResultOk; }
static tresult PAPI v3at_getf(void *self, const char *id, double *v) { v3_attr *a = attr_slot(self, id, 0); if (!a || a->kind != 2) return kResultFalse; *v = a->f; return kResultOk; }
static tresult PAPI v3at_setb(void *self, const char *id, const void *d, uint32_t n) { v3_attr *a = attr_slot(self, id, 1); if (!a) return kInvalidArgument; free(a->bin); a->bin = malloc(n ? n : 1); memcpy(a->bin, d, n); a->len = n; a->kind = 3; return kResultOk; }
static tresult PAPI v3at_getb(void *self, const char *id, const void **d, uint32_t *n) { v3_attr *a = attr_slot(self, id, 0); if (!a || a->kind < 3) return kResultFalse; *d = a->bin; *n = a->len; return kResultOk; }
static tresult PAPI v3at_sets(void *self, const char *id, const char16 *s) { uint32_t n = 0; while (s[n]) n++; tresult r = v3at_setb(self, id, s, (n + 1) * 2); if (r == kResultOk) attr_slot(self, id, 0)->kind = 4; return r; }
static tresult PAPI v3at_gets(void *self, const char *id, char16 *out, uint32_t bytes) {
  v3_attr *a = attr_slot(self, id, 0); if (!a || a->kind != 4 || !bytes) return kResultFalse;
  uint32_t k = a->len < bytes ? a->len : bytes; memcpy(out, a->bin, k); out[k / 2 - 1] = 0; return kResultOk;
}
static const struct v3_attrs_vt v3at_vt = { v3at_qi, v3at_add, v3at_rel, v3at_seti, v3at_geti, v3at_setf, v3at_getf, v3at_sets, v3at_gets, v3at_setb, v3at_getb };
static tresult PAPI v3ms_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IMessage)) { ((v3_message *)self)->refs++; *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static uint32_t PAPI v3ms_add(void *self) { return (uint32_t)++((v3_message *)self)->refs; }
static uint32_t PAPI v3ms_rel(void *self) { v3_message *m = self; if (--m->refs) return (uint32_t)m->refs; v3at_rel(m->attrs); free(m); return 0; }
static const char *PAPI v3ms_getid(void *self) { return ((v3_message *)self)->id; }
static void PAPI v3ms_setid(void *self, const char *id) { v3_message *m = self; size_t n = id ? strlen(id) : 0; if (n > 127) n = 127; memcpy(m->id, id ? id : "", n); m->id[n] = 0; }
static void *PAPI v3ms_attrs(void *self) { return ((v3_message *)self)->attrs; }
static const struct v3_message_vt v3ms_vt = { v3ms_qi, v3ms_add, v3ms_rel, v3ms_getid, v3ms_setid, v3ms_attrs };

typedef struct { const struct v3_hostapp_vt *vt; } v3_hostapp;
static tresult PAPI v3ha_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IHostApplication)) { *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static tresult PAPI v3ha_name(void *self, char16 *name) { (void)self; const char *n = "Undae Sonicae"; int i = 0; for (; n[i]; i++) name[i] = (char16)n[i]; name[i] = 0; return kResultOk; }
static tresult PAPI v3ha_create(void *self, const uint8_t *cid, const uint8_t *iid, void **obj) {
  (void)self;
  if (same_iid(cid, IID_IMessage) && same_iid(iid, IID_IMessage)) {
    v3_message *m = calloc(1, sizeof *m); m->vt = &v3ms_vt; m->refs = 1; m->attrs = calloc(1, sizeof *m->attrs); m->attrs->vt = &v3at_vt; m->attrs->refs = 1; *obj = m; return kResultOk;
  }
  if (same_iid(cid, IID_IAttributeList) && same_iid(iid, IID_IAttributeList)) { v3_attrs *l = calloc(1, sizeof *l); l->vt = &v3at_vt; l->refs = 1; *obj = l; return kResultOk; }
  *obj = NULL; return kNoInterface;
}
static const struct v3_hostapp_vt v3ha_vt = { v3ha_qi, ref_one, ref_one, v3ha_name, v3ha_create };
static v3_hostapp host_app = { &v3ha_vt };

// Parameter changes and events handed to process(): fixed-size, filled on the audio thread before each call.
typedef struct { const struct v3_queue_vt *vt; ParamID id; int32_t n; int32_t off[64]; double val[64]; } v3_queue;
typedef struct { const struct v3_changes_vt *vt; int32_t n; v3_queue q[64]; } v3_changes;
typedef struct { const struct v3_events_vt *vt; int32_t n; V3Event e[256]; } v3_events;
static tresult PAPI v3q_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IParamValueQueue)) { *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static ParamID PAPI v3q_id(void *self) { return ((v3_queue *)self)->id; }
static int32_t PAPI v3q_count(void *self) { return ((v3_queue *)self)->n; }
static tresult PAPI v3q_get(void *self, int32_t i, int32_t *off, ParamValue *v) { v3_queue *q = self; if (i < 0 || i >= q->n) return kInvalidArgument; *off = q->off[i]; *v = q->val[i]; return kResultOk; }
static tresult PAPI v3q_add(void *self, int32_t off, ParamValue v, int32_t *index) {
  v3_queue *q = self; if (q->n && q->off[q->n - 1] == off) { q->val[q->n - 1] = v; if (index) *index = q->n - 1; return kResultOk; }
  if (q->n == 64) return kResultFalse;
  q->off[q->n] = off; q->val[q->n] = v; if (index) *index = q->n; q->n++; return kResultOk;
}
static const struct v3_queue_vt v3q_vt = { v3q_qi, ref_one, ref_one, v3q_id, v3q_count, v3q_get, v3q_add };
static tresult PAPI v3pc_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IParameterChanges)) { *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static int32_t PAPI v3pc_count(void *self) { return ((v3_changes *)self)->n; }
static void *PAPI v3pc_get(void *self, int32_t i) { v3_changes *p = self; return i >= 0 && i < p->n ? &p->q[i] : NULL; }
static void *PAPI v3pc_add(void *self, const ParamID *id, int32_t *index) {
  v3_changes *p = self;
  for (int32_t i = 0; i < p->n; i++) if (p->q[i].id == *id) { if (index) *index = i; return &p->q[i]; }
  if (p->n == 64) return NULL;
  v3_queue *q = &p->q[p->n]; q->vt = &v3q_vt; q->id = *id; q->n = 0; if (index) *index = p->n; p->n++; return q;
}
static const struct v3_changes_vt v3pc_vt = { v3pc_qi, ref_one, ref_one, v3pc_count, v3pc_get, v3pc_add };
static tresult PAPI v3ev_qi(void *self, const uint8_t *iid, void **obj) { if (same_iid(iid, IID_FUnknown) || same_iid(iid, IID_IEventList)) { *obj = self; return kResultOk; } *obj = NULL; return kNoInterface; }
static int32_t PAPI v3ev_count(void *self) { return ((v3_events *)self)->n; }
static tresult PAPI v3ev_get(void *self, int32_t i, V3Event *e) { v3_events *l = self; if (i < 0 || i >= l->n) return kInvalidArgument; *e = l->e[i]; return kResultOk; }
static tresult PAPI v3ev_add(void *self, V3Event *e) { v3_events *l = self; if (l->n == 256) return kResultFalse; l->e[l->n++] = *e; return kResultOk; }
static const struct v3_events_vt v3ev_vt = { v3ev_qi, ref_one, ref_one, v3ev_count, v3ev_get, v3ev_add };

