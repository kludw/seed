/* Lists ([]T) and maps (map[K]V), with copy-on-write. */
#include "seed_internal.h"

/* ================================================================ lists */

static size_t list_bytes(const sd_type *et, int64_t cap) { return sizeof(sd_list) + (size_t)cap * et->size; }

/* The capacity of list l, allocated for cap elements (n bytes): as many
 * elements as its block holds. */
static int64_t list_cap(const sd_list *l, int64_t cap, size_t n) {
  size_t sz = l->et->size, extra = sd_mem_usable(l, n) - n;
  return sz && extra >= sz ? cap + (int64_t)(extra / sz) : cap;
}

sd_list *sd_list_new(const sd_type *et, int64_t cap) {
  if (cap < 0) cap = 0;
  size_t n = list_bytes(et, cap);
  sd_list *l = sd_obj_alloc(n);
  l->rc = 1;
  l->len = 0;
  l->et = et;
  l->cap = list_cap(l, cap, n);
  return l;
}

sd_list *sd_list_new_len_(const sd_type *et, int64_t len) {
  sd_list *l = sd_list_new(et, len);
  l->len = len;
  return l;
}

void sd_list_free_(sd_list *l) { sd_free_queued_(l, SD_DEAD_LIST); }

void sd_list_destroy_(sd_list *l) {
  const sd_type *et = l->et;
  if (!et->trivial) {
    char *d = sd_list_data(l);
    for (int64_t i = 0; i < l->len; i++) sd_release(et, d + (size_t)i * et->size);
  }
  sd_obj_free(l);
}

/* Makes *pl unshared (rc == 1), copying it when needed, with room for at
 * least len + extra elements in the copy. */
static sd_list *list_unique(sd_list **pl, int64_t extra) {
  sd_list *l = *pl;
  if (SD_LIKELY(l->rc == 1)) return l;
  const sd_type *et = l->et;
  sd_list *c = sd_list_new(et, l->len + extra);
  c->len = l->len;
  memcpy(sd_list_data(c), sd_list_data(l), (size_t)l->len * et->size);
  if (!et->trivial) {
    char *d = sd_list_data(c);
    for (int64_t i = 0; i < c->len; i++) sd_retain(et, d + (size_t)i * et->size);
  }
  sd_list_release(l); /* shared or immortal: never freed here */
  *pl = c;
  return c;
}

static sd_list *list_reserve(sd_list **pl, int64_t need) {
  sd_list *l = list_unique(pl, need - (*pl)->len);
  if (need > l->cap) {
    int64_t cap = l->cap ? l->cap * 2 : 4;
    while (cap < need) cap *= 2;
    size_t n = list_bytes(l->et, cap);
    l = sd_obj_realloc(l, n);
    l->cap = list_cap(l, cap, n);
    *pl = l;
  }
  return l;
}

void *sd_list_mut(sd_list **pl) { return sd_list_data(list_unique(pl, 0)); }

void *sd_list_at_mut(sd_list **pl, sd_int i) {
  int64_t k = sd_int_index(i, (*pl)->len);
  sd_list *l = list_unique(pl, 0);
  return (char *)sd_list_data(l) + (size_t)k * l->et->size;
}

/* A small buffer for a copy of one element (the value passed to a store may
 * point into the list being changed). */
typedef struct {
  void *p;
  uint64_t inline_[8];
} elem_tmp;

static void tmp_init(elem_tmp *t, const void *val, size_t size) {
  t->p = size <= sizeof t->inline_ ? (void *)t->inline_ : sd_alloc(size);
  memcpy(t->p, val, size);
}
static void tmp_done(elem_tmp *t) {
  if (t->p != (void *)t->inline_) free(t->p);
}

void sd_list_set(sd_list **pl, sd_int i, const void *val) {
  int64_t k = sd_int_index(i, (*pl)->len);
  size_t sz = (*pl)->et->size;
  elem_tmp t;
  tmp_init(&t, val, sz);
  sd_list *l = list_unique(pl, 0);
  char *slot = (char *)sd_list_data(l) + (size_t)k * sz;
  sd_release(l->et, slot);
  memcpy(slot, t.p, sz);
  tmp_done(&t);
}

void sd_list_push(sd_list **pl, const void *val) {
  size_t sz = (*pl)->et->size;
  elem_tmp t;
  tmp_init(&t, val, sz);
  sd_list *l = list_reserve(pl, (*pl)->len + 1);
  memcpy((char *)sd_list_data(l) + (size_t)l->len * sz, t.p, sz);
  l->len++;
  tmp_done(&t);
}

bool sd_list_pop(sd_list **pl, void *out) {
  if ((*pl)->len == 0) return false;
  sd_list *l = list_unique(pl, 0);
  l->len--;
  memcpy(out, (char *)sd_list_data(l) + (size_t)l->len * l->et->size, l->et->size);
  return true;
}

void sd_list_insert(sd_list **pl, sd_int i, const void *val) {
  int64_t k, len = (*pl)->len;
  if (!sd_int_to_i64(i, &k) || k < 0 || k > len) {
    char *s = sd_int_to_cstr(i);
    char *m = sd_fmt("insert index out of range: %s with length %lld", s, (long long)len);
    free(s);
    sd_crash_owned(m);
  }
  size_t sz = (*pl)->et->size;
  elem_tmp t;
  tmp_init(&t, val, sz);
  sd_list *l = list_reserve(pl, len + 1);
  char *d = sd_list_data(l);
  memmove(d + (size_t)(k + 1) * sz, d + (size_t)k * sz, (size_t)(len - k) * sz);
  memcpy(d + (size_t)k * sz, t.p, sz);
  l->len++;
  tmp_done(&t);
}

void sd_list_remove(sd_list **pl, sd_int i) {
  int64_t k = sd_int_index(i, (*pl)->len);
  sd_list *l = list_unique(pl, 0);
  size_t sz = l->et->size;
  char *d = sd_list_data(l);
  sd_release(l->et, d + (size_t)k * sz);
  memmove(d + (size_t)k * sz, d + (size_t)(k + 1) * sz, (size_t)(l->len - k - 1) * sz);
  l->len--;
}

static int64_t list_find(const sd_list *l, const void *x) {
  const sd_type *et = l->et;
  const char *d = sd_list_data(l);
  for (int64_t i = 0; i < l->len; i++)
    if (sd_equal(et, d + (size_t)i * et->size, x)) return i;
  return -1;
}

bool sd_list_contains(const sd_list *l, const void *x) { return list_find(l, x) >= 0; }

sd_int sd_list_index(const sd_list *l, const void *x) {
  int64_t i = list_find(l, x);
  return i < 0 ? SD_INT_NULL : SD_INT(i);
}

sd_list *sd_list_slice(const sd_list *l, sd_int a, sd_int b) {
  int64_t x, y;
  if (!sd_int_to_i64(a, &x) || !sd_int_to_i64(b, &y) || x < 0 || x > y || y > l->len) {
    char *s = sd_int_to_cstr(a), *t = sd_int_to_cstr(b);
    char *m = sd_fmt("slice bounds out of range: [%s:%s] with length %lld", s, t, (long long)l->len);
    free(s);
    free(t);
    sd_crash_owned(m);
  }
  const sd_type *et = l->et;
  sd_list *r = sd_list_new_len_(et, y - x);
  memcpy(sd_list_data(r), (const char *)sd_list_data(l) + (size_t)x * et->size, (size_t)(y - x) * et->size);
  if (!et->trivial) {
    char *d = sd_list_data(r);
    for (int64_t i = 0; i < r->len; i++) sd_retain(et, d + (size_t)i * et->size);
  }
  return r;
}

void sd_list_reverse(sd_list **pl) {
  if ((*pl)->len < 2) return;
  sd_list *l = list_unique(pl, 0);
  size_t sz = l->et->size;
  char *d = sd_list_data(l);
  elem_tmp t;
  tmp_init(&t, d, sz);
  for (int64_t i = 0, j = l->len - 1; i < j; i++, j--) {
    memcpy(t.p, d + (size_t)i * sz, sz);
    memcpy(d + (size_t)i * sz, d + (size_t)j * sz, sz);
    memcpy(d + (size_t)j * sz, t.p, sz);
  }
  tmp_done(&t);
}

typedef bool (*sd_less_fn)(void *ctx, const void *a, const void *b);

/* Stable merge sort of n elements of size sz. */
static void msort(char *a, char *tmp, int64_t n, size_t sz, sd_less_fn less, void *ctx) {
  if (n < 2) return;
  if (n <= 8) {
    for (int64_t i = 1; i < n; i++) {
      memcpy(tmp, a + (size_t)i * sz, sz);
      int64_t j = i;
      while (j > 0 && less(ctx, tmp, a + (size_t)(j - 1) * sz)) {
        memcpy(a + (size_t)j * sz, a + (size_t)(j - 1) * sz, sz);
        j--;
      }
      memcpy(a + (size_t)j * sz, tmp, sz);
    }
    return;
  }
  int64_t m = n / 2;
  msort(a, tmp, m, sz, less, ctx);
  msort(a + (size_t)m * sz, tmp, n - m, sz, less, ctx);
  if (!less(ctx, a + (size_t)m * sz, a + (size_t)(m - 1) * sz)) return; /* already in order */
  int64_t i = 0, j = m, k = 0;
  while (i < m && j < n) {
    if (less(ctx, a + (size_t)j * sz, a + (size_t)i * sz)) memcpy(tmp + (size_t)(k++) * sz, a + (size_t)(j++) * sz, sz);
    else memcpy(tmp + (size_t)(k++) * sz, a + (size_t)(i++) * sz, sz);
  }
  while (i < m) memcpy(tmp + (size_t)(k++) * sz, a + (size_t)(i++) * sz, sz);
  while (j < n) memcpy(tmp + (size_t)(k++) * sz, a + (size_t)(j++) * sz, sz);
  memcpy(a, tmp, (size_t)n * sz);
}

static void list_sort(sd_list **pl, sd_less_fn less, void *ctx) {
  if ((*pl)->len < 2) return;
  sd_list *l = list_unique(pl, 0);
  size_t sz = l->et->size;
  char *tmp = sd_alloc((size_t)l->len * sz);
  msort(sd_list_data(l), tmp, l->len, sz, less, ctx);
  free(tmp);
}

static bool less_int(void *ctx, const void *a, const void *b) {
  (void)ctx;
  return sd_int_lt(*(const sd_int *)a, *(const sd_int *)b);
}
static bool less_float(void *ctx, const void *a, const void *b) {
  (void)ctx;
  return *(const double *)a < *(const double *)b;
}
static bool less_byte(void *ctx, const void *a, const void *b) {
  (void)ctx;
  return *(const uint8_t *)a < *(const uint8_t *)b;
}
static bool less_str(void *ctx, const void *a, const void *b) {
  (void)ctx;
  return sd_str_cmp(*(sd_str const *)a, *(sd_str const *)b) < 0;
}
static bool less_generic(void *ctx, const void *a, const void *b) { return sd_compare(ctx, a, b) < 0; }

void sd_list_sort(sd_list **pl) {
  const sd_type *et = (*pl)->et;
  switch (et->kind) {
  case SD_K_INT: list_sort(pl, less_int, NULL); break;
  case SD_K_FLOAT: list_sort(pl, less_float, NULL); break;
  case SD_K_BYTE: list_sort(pl, less_byte, NULL); break;
  case SD_K_STR: list_sort(pl, less_str, NULL); break;
  default: list_sort(pl, less_generic, (void *)et); break;
  }
}

static void free_buffer(void *env) { free(*(void **)env); }

/* sortBy calls Seed code, which may crash or be stopped halfway through a
 * merge, when the elements are partly duplicated and partly missing. So it
 * sorts a bitwise copy and writes it back only when the sort is complete: a
 * crash leaves the list as it was (its locals' cleanup frames then release it
 * normally), and a deferred free releases the buffer. */
void sd_list_sort_by(sd_list **pl, bool (*less)(void *ctx, const void *a, const void *b), void *ctx) {
  if ((*pl)->len < 2) return;
  sd_list *l = list_unique(pl, 0);
  size_t sz = l->et->size, n = (size_t)l->len;
  char *buf = sd_alloc(2 * n * sz);
  size_t mark = sd_defer_mark();
  sd_defer_push(free_buffer, &buf, sizeof buf);
  memcpy(buf, sd_list_data(l), n * sz);
  msort(buf, buf + n * sz, l->len, sz, less, ctx);
  memcpy(sd_list_data(l), buf, n * sz);
  sd_defer_run(mark);
}

sd_str sd_list_join(const sd_list *l, sd_str sep) {
  sd_str const *xs = sd_list_data(l);
  if (l->len == 0) return sd_str_empty();
  if (l->len == 1) return sd_str_retain(xs[0]);
  int64_t n = sep->len * (l->len - 1);
  for (int64_t i = 0; i < l->len; i++) n += xs[i]->len;
  sd_str r = sd_str_alloc_(n);
  char *p = r->data;
  for (int64_t i = 0; i < l->len; i++) {
    if (i) {
      memcpy(p, sep->data, (size_t)sep->len);
      p += sep->len;
    }
    memcpy(p, xs[i]->data, (size_t)xs[i]->len);
    p += xs[i]->len;
  }
  return r;
}

bool sd_list_equal(const sd_list *a, const sd_list *b) {
  if (a == b) return true;
  if (!a || !b || a->len != b->len) return false;
  const sd_type *et = a->et;
  const char *x = sd_list_data(a), *y = sd_list_data(b);
  for (int64_t i = 0; i < a->len; i++)
    if (!sd_equal(et, x + (size_t)i * et->size, y + (size_t)i * et->size)) return false;
  return true;
}

sd_list *sd_list_clone_deep(const sd_list *l) {
  if (!l) return NULL;
  if (l->rc < 0) return (sd_list *)l; /* immortal (and so is everything inside) */
  const sd_type *et = l->et;
  sd_list *c = sd_list_new_len_(et, l->len);
  if (et->trivial) {
    memcpy(sd_list_data(c), sd_list_data(l), (size_t)l->len * et->size);
  } else {
    char *d = sd_list_data(c);
    const char *s = sd_list_data(l);
    for (int64_t i = 0; i < l->len; i++) sd_clone_deep(et, d + (size_t)i * et->size, s + (size_t)i * et->size);
  }
  return c;
}

/* ================================================================= maps */

#define ENT(m, i) ((m)->entries + (size_t)(i) * (size_t)(m)->esz)
#define EHASH(e) (*(uint64_t *)(void *)(e))
#define ELIVE(e) (*(uint64_t *)(void *)((e) + 8))
#define EKEY(m, e) ((e) + (m)->koff)
#define EVAL(m, e) ((e) + (m)->voff)

sd_map *sd_map_new(const sd_type *kt, const sd_type *vt) {
  sd_map *m = sd_obj_alloc(sizeof(sd_map));
  memset(m, 0, sizeof *m);
  m->rc = 1;
  m->kt = kt;
  m->vt = vt;
  m->koff = 16;
  m->voff = (int32_t)(16 + sd_round8(kt->size));
  m->esz = (int32_t)(m->voff + sd_round8(vt->size));
  return m;
}

sd_int sd_map_len(const sd_map *m) { return SD_INT(m->len); }

void sd_map_free_(sd_map *m) { sd_free_queued_(m, SD_DEAD_MAP); }

void sd_map_destroy_(sd_map *m) {
  if (!m->kt->trivial || !m->vt->trivial) {
    for (int64_t i = 0; i < m->n; i++) {
      char *e = ENT(m, i);
      if (!ELIVE(e)) continue;
      sd_release(m->kt, EKEY(m, e));
      sd_release(m->vt, EVAL(m, e));
    }
  }
  sd_mem_free(m->entries);
  sd_mem_free(m->index);
  sd_obj_free(m);
}

static bool key_eq(const sd_type *kt, const void *a, const void *b) {
  switch (kt->kind) {
  case SD_K_STR: return sd_str_eq(*(sd_str const *)a, *(sd_str const *)b);
  case SD_K_INT: return sd_int_eq(*(const sd_int *)a, *(const sd_int *)b);
  default: return sd_equal(kt, a, b);
  }
}

static uint64_t key_hash(const sd_type *kt, const void *k) {
  switch (kt->kind) {
  case SD_K_STR: return sd_str_hash(*(sd_str const *)k);
  case SD_K_INT: return sd_int_hash(*(const sd_int *)k);
  default: return sd_hash(kt, k);
  }
}

/* The index slot holding key, or -1. */
static int64_t map_probe(const sd_map *m, uint64_t h, const void *key) {
  if (!m->icap) return -1;
  uint64_t mask = (uint64_t)m->icap - 1, i = h & mask;
  for (;;) {
    int32_t x = m->index[i];
    if (x == 0) return -1;
    if (x > 0) {
      char *e = ENT(m, x - 1);
      if (EHASH(e) == h && key_eq(m->kt, EKEY(m, e), key)) return (int64_t)i;
    }
    i = (i + 1) & mask;
  }
}

static void map_index_add(sd_map *m, int64_t k) {
  uint64_t mask = (uint64_t)m->icap - 1, i = EHASH(ENT(m, k)) & mask;
  while (m->index[i] > 0) i = (i + 1) & mask;
  m->index[i] = (int32_t)(k + 1);
}

/* Compacts the live entries into arrays of ncap entries and rebuilds the index. */
static void map_rebuild(sd_map *m, int64_t ncap) {
  if (ncap > ((int64_t)1 << 29)) sd_crash("map too large");
  char *ne = sd_mem_alloc((size_t)ncap * (size_t)m->esz);
  int64_t j = 0;
  for (int64_t i = 0; i < m->n; i++) {
    char *e = ENT(m, i);
    if (!ELIVE(e)) continue;
    memcpy(ne + (size_t)j * (size_t)m->esz, e, (size_t)m->esz);
    j++;
  }
  sd_mem_free(m->entries);
  sd_mem_free(m->index);
  m->entries = ne;
  m->n = j;
  m->cap = ncap;
  m->icap = ncap * 2;
  m->index = sd_mem_alloc((size_t)m->icap * sizeof(int32_t));
  memset(m->index, 0, (size_t)m->icap * sizeof(int32_t));
  for (int64_t i = 0; i < j; i++) map_index_add(m, i);
}

static sd_map *map_copy(const sd_map *m) {
  sd_map *c = sd_obj_alloc(sizeof(sd_map));
  *c = *m;
  c->rc = 1;
  c->entries = NULL;
  c->index = NULL;
  if (m->cap) {
    c->entries = sd_mem_alloc((size_t)m->cap * (size_t)m->esz);
    memcpy(c->entries, m->entries, (size_t)m->n * (size_t)m->esz);
    c->index = sd_mem_alloc((size_t)m->icap * sizeof(int32_t));
    memcpy(c->index, m->index, (size_t)m->icap * sizeof(int32_t));
  }
  if (!m->kt->trivial || !m->vt->trivial) {
    for (int64_t i = 0; i < c->n; i++) {
      char *e = ENT(c, i);
      if (!ELIVE(e)) continue;
      sd_retain(c->kt, EKEY(c, e));
      sd_retain(c->vt, EVAL(c, e));
    }
  }
  return c;
}

static sd_map *map_unique(sd_map **pm) {
  sd_map *m = *pm;
  if (SD_LIKELY(m->rc == 1)) return m;
  sd_map *c = map_copy(m);
  sd_map_release(m);
  *pm = c;
  return c;
}

void *sd_map_get(const sd_map *m, const void *key) {
  int64_t s = map_probe(m, key_hash(m->kt, key), key);
  return s < 0 ? NULL : EVAL(m, ENT(m, m->index[s] - 1));
}

void *sd_map_get_mut(sd_map **pm, const void *key) {
  uint64_t h = key_hash((*pm)->kt, key);
  if (map_probe(*pm, h, key) < 0) return NULL;
  sd_map *m = map_unique(pm);
  int64_t s = map_probe(m, h, key);
  return EVAL(m, ENT(m, m->index[s] - 1));
}

void sd_map_set(sd_map **pm, const void *key, const void *val) {
  const sd_type *kt = (*pm)->kt, *vt = (*pm)->vt;
  uint64_t kbuf[2];
  memcpy(kbuf, key, kt->size);
  elem_tmp t;
  tmp_init(&t, val, vt->size);
  uint64_t h = key_hash(kt, kbuf);
  sd_map *m = map_unique(pm);
  int64_t s = map_probe(m, h, kbuf);
  if (s >= 0) {
    char *v = EVAL(m, ENT(m, m->index[s] - 1));
    sd_release(vt, v);
    memcpy(v, t.p, vt->size);
    sd_release(kt, kbuf); /* the key was consumed but is not stored */
  } else {
    if (m->n == m->cap) map_rebuild(m, m->len * 2 >= m->cap ? (m->cap ? m->cap * 2 : 8) : m->cap);
    int64_t k = m->n++;
    char *e = ENT(m, k);
    EHASH(e) = h;
    ELIVE(e) = 1;
    memcpy(EKEY(m, e), kbuf, kt->size);
    memcpy(EVAL(m, e), t.p, vt->size);
    m->len++;
    map_index_add(m, k);
  }
  tmp_done(&t);
}

void sd_map_delete(sd_map **pm, const void *key) {
  uint64_t h = key_hash((*pm)->kt, key);
  if (map_probe(*pm, h, key) < 0) return;
  sd_map *m = map_unique(pm);
  int64_t s = map_probe(m, h, key);
  char *e = ENT(m, m->index[s] - 1);
  sd_release(m->kt, EKEY(m, e));
  sd_release(m->vt, EVAL(m, e));
  ELIVE(e) = 0;
  m->index[s] = -1;
  m->len--;
}

sd_list *sd_map_keys(const sd_map *m) {
  const sd_type *kt = m->kt;
  sd_list *l = sd_list_new_len_(kt, m->len);
  char *d = sd_list_data(l);
  int64_t j = 0;
  for (int64_t i = 0; i < m->n; i++) {
    char *e = ENT(m, i);
    if (!ELIVE(e)) continue;
    memcpy(d + (size_t)j * kt->size, EKEY(m, e), kt->size);
    sd_retain(kt, d + (size_t)j * kt->size);
    j++;
  }
  return l;
}

bool sd_map_next(const sd_map *m, int64_t *pos, void **key, void **val) {
  while (*pos < m->n) {
    char *e = ENT(m, *pos);
    (*pos)++;
    if (!ELIVE(e)) continue;
    if (key) *key = EKEY(m, e);
    if (val) *val = EVAL(m, e);
    return true;
  }
  return false;
}

bool sd_map_equal(const sd_map *a, const sd_map *b) {
  if (a == b) return true;
  if (!a || !b || a->len != b->len) return false;
  for (int64_t i = 0; i < a->n; i++) {
    char *e = ENT(a, i);
    if (!ELIVE(e)) continue;
    int64_t s = map_probe(b, EHASH(e), EKEY(a, e));
    if (s < 0) return false;
    if (!sd_equal(a->vt, EVAL(a, e), EVAL(b, ENT(b, b->index[s] - 1)))) return false;
  }
  return true;
}

sd_map *sd_map_clone_deep(const sd_map *m) {
  if (!m) return NULL;
  if (m->rc < 0) return (sd_map *)m;
  sd_map *c = sd_obj_alloc(sizeof(sd_map));
  *c = *m;
  c->rc = 1;
  c->entries = NULL;
  c->index = NULL;
  c->n = c->len = c->cap = c->icap = 0;
  if (m->len) {
    int64_t cap = 8;
    while (cap < m->len) cap *= 2;
    c->entries = sd_mem_alloc((size_t)cap * (size_t)m->esz);
    c->cap = cap;
    for (int64_t i = 0; i < m->n; i++) {
      char *e = ENT(m, i);
      if (!ELIVE(e)) continue;
      char *d = ENT(c, c->n);
      EHASH(d) = EHASH(e);
      ELIVE(d) = 1;
      sd_clone_deep(m->kt, EKEY(c, d), EKEY(m, e));
      sd_clone_deep(m->vt, EVAL(c, d), EVAL(m, e));
      c->n++;
    }
    c->len = c->n;
    c->icap = cap * 2;
    c->index = sd_mem_alloc((size_t)c->icap * sizeof(int32_t));
    memset(c->index, 0, (size_t)c->icap * sizeof(int32_t));
    for (int64_t i = 0; i < c->n; i++) map_index_add(c, i);
  }
  return c;
}
