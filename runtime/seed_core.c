/* Allocation, type descriptors, boxes, interface values and errors. */
#include "seed_internal.h"

#include <unistd.h>

/* ---------------------------------------------------------------- memory */

#if defined(SD_COUNT_OBJECTS)
_Atomic int64_t sd_live_count_;
int64_t sd_live_objects(void) { return atomic_load(&sd_live_count_); }
#else
int64_t sd_live_objects(void) { return -1; }
#endif

void sd_fatal(const char *msg) {
  fflush(stdout);
  fprintf(stderr, "crash: %s\n", msg);
  fflush(stderr);
  _exit(2);
}

void *sd_alloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (SD_UNLIKELY(!p)) sd_fatal("out of memory");
  return p;
}

void *sd_realloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (SD_UNLIKELY(!p)) sd_fatal("out of memory");
  return p;
}

void sd_free(void *p) { free(p); }

char *sd_vfmt(const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  char tmp[256];
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  if (n < 0) n = 0;
  char *s = sd_alloc((size_t)n + 1);
  if ((size_t)n < sizeof tmp) {
    memcpy(s, tmp, (size_t)n + 1);
  } else {
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
  }
  va_end(ap2);
  return s;
}

char *sd_fmt(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = sd_vfmt(fmt, ap);
  va_end(ap);
  return s;
}

/* ---------------------------------------------------- built-in descriptors */

const sd_type sd_type_int = {.name = "int", .size = sizeof(sd_int), .kind = SD_K_INT};
const sd_type sd_type_float = {.name = "float", .size = sizeof(double), .kind = SD_K_FLOAT, .trivial = true};
const sd_type sd_type_bool = {.name = "bool", .size = sizeof(bool), .kind = SD_K_BOOL, .trivial = true};
const sd_type sd_type_byte = {.name = "byte", .size = sizeof(uint8_t), .kind = SD_K_BYTE, .trivial = true};
const sd_type sd_type_fn = {.name = "fn", .size = sizeof(void (*)(void)), .kind = SD_K_FN, .trivial = true};
const sd_type sd_type_str = {.name = "string", .size = sizeof(sd_str), .kind = SD_K_STR};
const sd_type sd_type_list = {.name = "list", .size = sizeof(sd_list *), .kind = SD_K_LIST};
const sd_type sd_type_map = {.name = "map", .size = sizeof(sd_map *), .kind = SD_K_MAP};
const sd_type sd_type_box = {.name = "box", .size = sizeof(sd_box *), .kind = SD_K_BOX};
const sd_type sd_type_chan = {.name = "chan", .size = sizeof(sd_chan *), .kind = SD_K_CHAN};
const sd_type sd_type_iface = {.name = "interface", .size = sizeof(sd_iface), .kind = SD_K_IFACE};
const sd_type sd_type_err = {.name = "error", .size = sizeof(sd_err), .kind = SD_K_ERR};
const sd_type sd_type_opt_float = {.name = "float?", .size = sizeof(sd_opt_float), .kind = SD_K_OPT_FLOAT, .trivial = true};
const sd_type sd_type_opt_bool = {.name = "bool?", .size = sizeof(sd_opt_bool), .kind = SD_K_OPT_BOOL, .trivial = true};
const sd_type sd_type_opt_byte = {.name = "byte?", .size = sizeof(sd_opt_byte), .kind = SD_K_OPT_BYTE, .trivial = true};

/* ------------------------------------------------------ generic dispatch */

bool sd_type_is_trivial(const sd_type *t) { return t->trivial; }

static const sd_variant *sd_enum_variant(const sd_type *t, const void *p) {
  int32_t tag = *(const int32_t *)p;
  if (SD_UNLIKELY(tag < 0 || tag >= t->nvariants)) sd_fatal("corrupt enum tag");
  return &t->variants[tag];
}

void sd_retain(const sd_type *t, void *p) {
  if (t->trivial) return;
  switch (t->kind) {
  case SD_K_INT: sd_int_retain(*(sd_int *)p); break;
  case SD_K_STR: sd_str_retain(*(sd_str *)p); break;
  case SD_K_LIST: sd_list_retain(*(sd_list **)p); break;
  case SD_K_MAP: sd_map_retain(*(sd_map **)p); break;
  case SD_K_BOX: sd_box_retain(*(sd_box **)p); break;
  case SD_K_CHAN: if (*(sd_chan **)p) sd_chan_retain(*(sd_chan **)p); break;
  case SD_K_IFACE:
  case SD_K_ERR: sd_box_retain(((sd_iface *)p)->obj); break;
  case SD_K_STRUCT:
    for (int32_t i = 0; i < t->nfields; i++) sd_retain(t->fields[i].type, (char *)p + t->fields[i].offset);
    break;
  case SD_K_ENUM: {
    const sd_variant *v = sd_enum_variant(t, p);
    for (int32_t i = 0; i < v->nfields; i++) sd_retain(v->fields[i].type, (char *)p + v->fields[i].offset);
    break;
  }
  case SD_K_CUSTOM: t->retain(p); break;
  default: break;
  }
}

void sd_release(const sd_type *t, void *p) {
  if (t->trivial) return;
  switch (t->kind) {
  case SD_K_INT: sd_int_release(*(sd_int *)p); break;
  case SD_K_STR: sd_str_release(*(sd_str *)p); break;
  case SD_K_LIST: sd_list_release(*(sd_list **)p); break;
  case SD_K_MAP: sd_map_release(*(sd_map **)p); break;
  case SD_K_BOX: sd_box_release(*(sd_box **)p); break;
  case SD_K_CHAN: if (*(sd_chan **)p) sd_chan_release(*(sd_chan **)p); break;
  case SD_K_IFACE:
  case SD_K_ERR: sd_box_release(((sd_iface *)p)->obj); break;
  case SD_K_STRUCT:
    for (int32_t i = 0; i < t->nfields; i++) sd_release(t->fields[i].type, (char *)p + t->fields[i].offset);
    break;
  case SD_K_ENUM: {
    const sd_variant *v = sd_enum_variant(t, p);
    for (int32_t i = 0; i < v->nfields; i++) sd_release(v->fields[i].type, (char *)p + v->fields[i].offset);
    break;
  }
  case SD_K_CUSTOM: t->release(p); break;
  default: break;
  }
}

static sd_int sd_int_clone_deep(sd_int a) {
  if ((a.w & 1) == 0 || a.w == 1) return a;
  sd_bigint *b = sd_int_big_(a);
  if (b->rc < 0) return a; /* immortal: shareable */
  return (sd_int){(int64_t)(uintptr_t)sd_bigint_copy_(b) | 1};
}

static sd_str sd_str_clone_deep(sd_str s) {
  if (!s || s->rc < 0) return s;
  return sd_str_new(s->data, s->len);
}

void sd_clone_deep(const sd_type *t, void *dst, const void *src) {
  if (t->trivial) {
    memcpy(dst, src, t->size);
    return;
  }
  switch (t->kind) {
  case SD_K_INT: *(sd_int *)dst = sd_int_clone_deep(*(const sd_int *)src); break;
  case SD_K_STR: *(sd_str *)dst = sd_str_clone_deep(*(sd_str const *)src); break;
  case SD_K_LIST: *(sd_list **)dst = sd_list_clone_deep(*(sd_list *const *)src); break;
  case SD_K_MAP: *(sd_map **)dst = sd_map_clone_deep(*(sd_map *const *)src); break;
  case SD_K_BOX: *(sd_box **)dst = sd_box_clone_deep(*(sd_box *const *)src); break;
  case SD_K_CHAN: {
    sd_chan *c = *(sd_chan *const *)src;
    *(sd_chan **)dst = c ? sd_chan_retain(c) : NULL; /* channels are shared */
    break;
  }
  case SD_K_IFACE:
  case SD_K_ERR: {
    const sd_iface *s = src;
    sd_iface *d = dst;
    d->vt = s->vt;
    d->obj = sd_box_clone_deep(s->obj);
    break;
  }
  case SD_K_STRUCT:
    memcpy(dst, src, t->size); /* padding and trivial fields */
    for (int32_t i = 0; i < t->nfields; i++)
      sd_clone_deep(t->fields[i].type, (char *)dst + t->fields[i].offset, (const char *)src + t->fields[i].offset);
    break;
  case SD_K_ENUM: {
    memcpy(dst, src, t->size);
    const sd_variant *v = sd_enum_variant(t, src);
    for (int32_t i = 0; i < v->nfields; i++)
      sd_clone_deep(v->fields[i].type, (char *)dst + v->fields[i].offset, (const char *)src + v->fields[i].offset);
    break;
  }
  case SD_K_CUSTOM: t->clone_deep(dst, src); break;
  default: memcpy(dst, src, t->size); break;
  }
}

bool sd_equal(const sd_type *t, const void *a, const void *b) {
  switch (t->kind) {
  case SD_K_FLOAT: return *(const double *)a == *(const double *)b;
  case SD_K_BOOL: return *(const bool *)a == *(const bool *)b;
  case SD_K_BYTE: return *(const uint8_t *)a == *(const uint8_t *)b;
  case SD_K_FN: return memcmp(a, b, sizeof(void (*)(void))) == 0;
  case SD_K_INT: return sd_int_eq(*(const sd_int *)a, *(const sd_int *)b);
  case SD_K_STR: return sd_str_eq(*(sd_str const *)a, *(sd_str const *)b);
  case SD_K_LIST: return sd_list_equal(*(sd_list *const *)a, *(sd_list *const *)b);
  case SD_K_MAP: return sd_map_equal(*(sd_map *const *)a, *(sd_map *const *)b);
  case SD_K_BOX: return sd_box_equal(*(sd_box *const *)a, *(sd_box *const *)b);
  case SD_K_CHAN: return *(sd_chan *const *)a == *(sd_chan *const *)b;
  case SD_K_IFACE: return sd_iface_equal(*(const sd_iface *)a, *(const sd_iface *)b);
  case SD_K_ERR: return sd_err_equal(*(const sd_err *)a, *(const sd_err *)b);
  case SD_K_OPT_FLOAT: {
    const sd_opt_float *x = a, *y = b;
    return x->has == y->has && (!x->has || x->v == y->v);
  }
  case SD_K_OPT_BOOL: {
    const sd_opt_bool *x = a, *y = b;
    return x->has == y->has && (!x->has || x->v == y->v);
  }
  case SD_K_OPT_BYTE: {
    const sd_opt_byte *x = a, *y = b;
    return x->has == y->has && (!x->has || x->v == y->v);
  }
  case SD_K_STRUCT:
    for (int32_t i = 0; i < t->nfields; i++)
      if (!sd_equal(t->fields[i].type, (const char *)a + t->fields[i].offset, (const char *)b + t->fields[i].offset))
        return false;
    return true;
  case SD_K_ENUM: {
    if (*(const int32_t *)a != *(const int32_t *)b) return false;
    const sd_variant *v = sd_enum_variant(t, a);
    for (int32_t i = 0; i < v->nfields; i++)
      if (!sd_equal(v->fields[i].type, (const char *)a + v->fields[i].offset, (const char *)b + v->fields[i].offset))
        return false;
    return true;
  }
  case SD_K_CUSTOM: return t->equal(a, b);
  }
  return false;
}

int sd_compare(const sd_type *t, const void *a, const void *b) {
  switch (t->kind) {
  case SD_K_INT: return sd_int_cmp(*(const sd_int *)a, *(const sd_int *)b);
  case SD_K_FLOAT: {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
  }
  case SD_K_BYTE: {
    uint8_t x = *(const uint8_t *)a, y = *(const uint8_t *)b;
    return (x > y) - (x < y);
  }
  case SD_K_STR: return sd_str_cmp(*(sd_str const *)a, *(sd_str const *)b);
  case SD_K_BOOL: return (int)*(const bool *)a - (int)*(const bool *)b;
  default: sd_crash("values of type %s cannot be ordered", t->name);
  }
}

static uint64_t sd_mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

uint64_t sd_hash(const sd_type *t, const void *p) {
  switch (t->kind) {
  case SD_K_INT: return sd_int_hash(*(const sd_int *)p);
  case SD_K_BYTE: return sd_mix64(*(const uint8_t *)p);
  case SD_K_BOOL: return sd_mix64(*(const bool *)p ? 1 : 0);
  case SD_K_STR: return sd_str_hash(*(sd_str const *)p);
  default: sd_crash("values of type %s cannot be map keys", t->name);
  }
}

uint64_t sd_int_hash(sd_int a) {
  if ((a.w & 1) == 0) return sd_mix64((uint64_t)a.w);
  sd_bigint *b = sd_int_big_(a);
  uint64_t h = (uint64_t)b->sign;
  for (int32_t i = 0; i < b->n; i++) h = sd_mix64(h ^ b->d[i]);
  return h;
}

/* ------------------------------------------------------------------ boxes */

sd_box *sd_box_new(const sd_type *t, const void *val) {
  sd_box *b = sd_obj_alloc(sizeof(sd_box) + t->size);
  b->rc = 1;
  b->t = t;
  memcpy(sd_box_data(b), val, t->size);
  return b;
}

void sd_box_free_(sd_box *b) {
  sd_release(b->t, sd_box_data(b));
  sd_obj_free(b);
}

void *sd_box_mut(sd_box **pb) {
  sd_box *b = *pb;
  if (SD_UNLIKELY(b->rc != 1)) {
    sd_box *c = sd_obj_alloc(sizeof(sd_box) + b->t->size);
    c->rc = 1;
    c->t = b->t;
    memcpy(sd_box_data(c), sd_box_data(b), b->t->size);
    sd_retain(b->t, sd_box_data(c));
    sd_box_release(b);
    *pb = c;
    b = c;
  }
  return sd_box_data(b);
}

bool sd_box_equal(const sd_box *a, const sd_box *b) {
  if (a == b) return true;
  if (!a || !b || a->t != b->t) return false;
  return sd_equal(a->t, sd_box_data(a), sd_box_data(b));
}

sd_box *sd_box_clone_deep(const sd_box *b) {
  if (!b) return NULL;
  if (b->rc < 0) return (sd_box *)b; /* immortal */
  sd_box *c = sd_obj_alloc(sizeof(sd_box) + b->t->size);
  c->rc = 1;
  c->t = b->t;
  sd_clone_deep(b->t, sd_box_data(c), sd_box_data(b));
  return c;
}

/* ------------------------------------------------------------- interfaces */

sd_iface sd_iface_new(const sd_vtable *vt, const sd_type *t, const void *val) {
  return (sd_iface){vt, sd_box_new(t, val)};
}

bool sd_iface_equal(sd_iface a, sd_iface b) {
  if (!a.vt || !b.vt) return a.vt == b.vt;
  return sd_box_equal(a.obj, b.obj);
}

/* ----------------------------------------------------------------- errors */

static sd_str sd_errors_message(const void *self) { return sd_str_retain(*(sd_str const *)self); }

const sd_error_vtable sd_errors_vtable = {{"errors.Error"}, sd_errors_message};

sd_err sd_errors_new(sd_str msg) {
  sd_str m = sd_str_retain(msg);
  return sd_iface_new(&sd_errors_vtable.hdr, &sd_type_str, &m);
}

sd_err sd_errorf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *s = sd_vfmt(fmt, ap);
  va_end(ap);
  sd_str m = sd_str_from_cstr(s);
  free(s);
  return sd_iface_new(&sd_errors_vtable.hdr, &sd_type_str, &m);
}

sd_str sd_err_message(sd_err e) {
  if (!e.vt) sd_crash("message() called on a null error");
  return ((const sd_error_vtable *)e.vt)->message(sd_box_data(e.obj));
}

bool sd_err_equal(sd_err a, sd_err b) {
  if (!a.vt || !b.vt) return a.vt == b.vt;
  if (a.obj == b.obj) return true;
  sd_str x = sd_err_message(a), y = sd_err_message(b);
  bool eq = sd_str_eq(x, y);
  sd_str_release(x);
  sd_str_release(y);
  return eq;
}
