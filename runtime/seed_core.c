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

/* ------------------------------------------------ small-object allocator
 *
 * Counted objects come from here (sd_obj_alloc); everything else uses
 * malloc. runtime/README.md, section 15, has the design; in short:
 *
 * - Every block has an 8-byte header word just before the payload: its size
 *   class, or 0 for a large block (malloc'ed, payload 16 bytes into it).
 *   Small blocks are slots of 16..2048 bytes (header included) carved from
 *   1 MB chunks; a free slot keeps its header and links to the next free
 *   slot through its first payload word.
 * - Each thread has a cache: a free list per class. alloc pops, free pushes,
 *   onto the lists of the thread that calls them, whichever thread allocated
 *   the block. A list that grows past 2 * batch moves a batch to the
 *   central pool; an empty list takes a batch from it (or carves a new one).
 *   The central pool is a stack of batches per class under a mutex, so
 *   blocks freed on one thread are reused by every other.
 * - A thread's cache goes back to the central pool when the thread ends.
 * - Chunks are never returned to the system; their slots are reused, so
 *   memory stays at the program's peak of live small objects plus caches.
 *
 * The thread-local cache pointer is read only inside sd_mem_alloc and
 * sd_mem_free, which are never inlined and never park, so no caller can
 * hold a thread-local address across a park (and a goroutine that moves
 * between two calls simply uses the other thread's cache). */

#if !defined(SD_PLAIN_MALLOC)
#include <pthread.h>

enum { SD_NCLASS = 25, SD_SMALL_MAX = 2048 - 8, SD_CHUNK = 1 << 20 };

/* Slot size (header included) of each class; class 0 is "large". */
static const uint16_t sd_class_slot[SD_NCLASS] = {0,   16,  32,  48,  64,  80,  96,   112,  128,  160,  192,  224, 256,
                                                  320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048};
/* Blocks moved between a thread cache and the central pool at a time. */
static const uint8_t sd_class_batch[SD_NCLASS] = {0,  64, 64, 64, 64, 64, 64, 64, 64, 51, 42, 36, 32,
                                                  25, 21, 18, 16, 12, 10, 9,  8,  6,  5,  4,  4};
/* Class of a block of n bytes (header included), by (n + 15) / 16. */
static const uint8_t sd_size_class[129] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15,
    15, 15, 16, 16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19,
    19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
    22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
    23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24};

#define SD_HDR(p) (((uint64_t *)(p))[-1])
#define SD_NEXT(p) (*(void **)(p))

typedef struct {
  void *head;
  int64_t room; /* 2 * batch - the number of blocks on the list */
} sd_flist;

typedef struct sd_tcache {
  sd_flist l[SD_NCLASS];
  struct sd_tcache *next_free; /* in sd_tcaches_free while no thread uses it */
} sd_tcache;

/* The calling thread's cache, NULL until its first allocation or free. */
static _Thread_local sd_tcache *sd_tc_;

typedef struct {
  pthread_mutex_t mu;
  void *batches; /* full batches, linked through their first block's header */
  void *loose;   /* single blocks (from caches of threads that ended) */
  int64_t nloose;
} sd_central;

static sd_central sd_central_[SD_NCLASS];
static pthread_mutex_t sd_chunk_mu = PTHREAD_MUTEX_INITIALIZER;
static char *sd_chunk_cur, *sd_chunk_end;
static sd_tcache *sd_tcaches_free; /* under sd_chunk_mu */
static pthread_once_t sd_mem_once = PTHREAD_ONCE_INIT;
static pthread_key_t sd_tc_key;

static void sd_tcache_exit(void *arg);

static void sd_mem_init(void) {
  for (int c = 1; c < SD_NCLASS; c++) pthread_mutex_init(&sd_central_[c].mu, NULL);
  if (pthread_key_create(&sd_tc_key, sd_tcache_exit) != 0) sd_fatal("cannot create the allocator's thread key");
}

/* The calling thread's cache. Only for sd_mem_alloc, sd_mem_free and their
 * slow paths: the asm hides the address from the optimiser, so it is read
 * from the thread pointer at each call. */
static inline sd_tcache *sd_tcache_peek(void) {
  sd_tcache **pp = &sd_tc_;
  __asm__ volatile("" : "+r"(pp));
  return *pp;
}

static sd_tcache *sd_tcache_new(void) {
  pthread_once(&sd_mem_once, sd_mem_init);
  pthread_mutex_lock(&sd_chunk_mu);
  sd_tcache *tc = sd_tcaches_free;
  if (tc) sd_tcaches_free = tc->next_free;
  pthread_mutex_unlock(&sd_chunk_mu);
  if (!tc) {
    tc = calloc(1, sizeof *tc);
    if (!tc) sd_fatal("out of memory");
    for (int c = 1; c < SD_NCLASS; c++) tc->l[c].room = 2 * sd_class_batch[c];
  }
  tc->next_free = NULL;
  sd_tcache **pp = &sd_tc_;
  __asm__ volatile("" : "+r"(pp));
  *pp = tc;
  /* The key's destructor returns the cache when the thread ends. */
  pthread_setspecific(sd_tc_key, tc);
  return tc;
}

/* n blocks of class c, linked, carved from the current chunk. */
static void *sd_carve(int c, int n) {
  size_t slot = sd_class_slot[c], need = slot * (size_t)n;
  pthread_mutex_lock(&sd_chunk_mu);
  if ((size_t)(sd_chunk_end - sd_chunk_cur) < need) {
    /* The rest of the old chunk (less than one batch) is not used. */
    char *ch = malloc(SD_CHUNK);
    if (!ch) sd_fatal("out of memory");
    /* malloc's 16-byte alignment: slots start 8 bytes past a multiple of
     * 16, so that payloads are 16-byte aligned. */
    sd_chunk_cur = ch + 8;
    sd_chunk_end = ch + SD_CHUNK - 8;
  }
  char *base = sd_chunk_cur;
  sd_chunk_cur += need;
  pthread_mutex_unlock(&sd_chunk_mu);
  for (int i = 0; i < n; i++) {
    char *p = base + (size_t)i * slot + 8;
    SD_HDR(p) = (uint64_t)c;
    SD_NEXT(p) = i + 1 < n ? p + slot : NULL;
  }
  return base + 8;
}

/* Moves the first n blocks of l (which has more than n) to the central
 * pool as one batch. */
static __attribute__((noinline)) void sd_flush_batch(sd_flist *l, int c, int n) {
  void *first = l->head, *last = first;
  for (int i = 1; i < n; i++) last = SD_NEXT(last);
  l->head = SD_NEXT(last);
  l->room += n;
  SD_NEXT(last) = NULL;
  sd_central *z = &sd_central_[c];
  pthread_mutex_lock(&z->mu);
  SD_HDR(first) = (uint64_t)(uintptr_t)z->batches;
  z->batches = first;
  pthread_mutex_unlock(&z->mu);
}

/* Fills the empty list l of class c with a batch from the central pool, or
 * a new one. */
static void sd_refill(sd_flist *l, int c) {
  int n = sd_class_batch[c];
  sd_central *z = &sd_central_[c];
  void *b = NULL;
  int got = 0;
  pthread_mutex_lock(&z->mu);
  if (z->batches) {
    b = z->batches;
    z->batches = (void *)(uintptr_t)SD_HDR(b);
    got = n;
  } else if (z->loose) {
    void *last = b = z->loose;
    got = 1;
    while (got < n && SD_NEXT(last)) {
      last = SD_NEXT(last);
      got++;
    }
    z->loose = SD_NEXT(last);
    z->nloose -= got;
    SD_NEXT(last) = NULL;
  }
  pthread_mutex_unlock(&z->mu);
  if (!b) {
    b = sd_carve(c, n);
    got = n;
  }
  SD_HDR(b) = (uint64_t)c; /* the batch link of a central batch */
  l->head = b;
  l->room -= got;
}

/* The thread's cache when the thread ends: its blocks go to the central
 * pool (full batches, the rest loose) and the cache to sd_tcaches_free. */
static void sd_tcache_exit(void *arg) {
  sd_tcache *tc = arg;
  for (int c = 1; c < SD_NCLASS; c++) {
    sd_flist *l = &tc->l[c];
    int n = sd_class_batch[c];
    while (2 * n - l->room > n) sd_flush_batch(l, c, n);
    if (l->head) {
      void *last = l->head;
      while (SD_NEXT(last)) last = SD_NEXT(last);
      sd_central *z = &sd_central_[c];
      pthread_mutex_lock(&z->mu);
      SD_NEXT(last) = z->loose;
      z->loose = l->head;
      z->nloose += 2 * n - l->room;
      pthread_mutex_unlock(&z->mu);
    }
    l->head = NULL;
    l->room = 2 * n;
  }
  /* A later allocation on this thread (from another key's destructor)
   * starts a new cache, which this destructor then runs for again. */
  sd_tcache **pp = &sd_tc_;
  __asm__ volatile("" : "+r"(pp));
  if (*pp == tc) *pp = NULL;
  pthread_mutex_lock(&sd_chunk_mu);
  tc->next_free = sd_tcaches_free;
  sd_tcaches_free = tc;
  pthread_mutex_unlock(&sd_chunk_mu);
}

static __attribute__((noinline)) void *sd_mem_alloc_large(size_t n) {
  if (n > SIZE_MAX - 16) sd_fatal("out of memory");
  char *b = malloc(n + 16);
  if (SD_UNLIKELY(!b)) sd_fatal("out of memory");
  SD_HDR(b + 16) = 0;
  return b + 16;
}

static __attribute__((noinline)) void *sd_mem_alloc_slow(int c) {
  sd_tcache *tc = sd_tcache_peek();
  if (!tc) tc = sd_tcache_new();
  sd_flist *l = &tc->l[c];
  if (!l->head) sd_refill(l, c);
  void *p = l->head;
  l->head = SD_NEXT(p);
  l->room++;
  return p;
}

void *sd_mem_alloc(size_t n) {
  if (SD_UNLIKELY(n > SD_SMALL_MAX)) return sd_mem_alloc_large(n);
  int c = sd_size_class[(n + 8 + 15) >> 4];
  sd_tcache *tc = sd_tcache_peek();
  void *p;
  if (SD_LIKELY(tc && (p = tc->l[c].head))) {
    tc->l[c].head = SD_NEXT(p);
    tc->l[c].room++;
  } else {
    p = sd_mem_alloc_slow(c);
  }
  SD_HDR(p) = (uint64_t)c;
  return p;
}

static __attribute__((noinline)) void sd_mem_free_slow(void *p, uint64_t c) {
  if (c == 0) {
    free((char *)p - 16);
    return;
  }
  if (c >= SD_NCLASS) sd_fatal("corrupt heap: bad block header (double free?)");
  sd_tcache *tc = sd_tcache_peek();
  if (!tc) tc = sd_tcache_new();
  sd_flist *l = &tc->l[c];
  SD_NEXT(p) = l->head;
  l->head = p;
  if (--l->room < 0) sd_flush_batch(l, (int)c, sd_class_batch[c]);
}

void sd_mem_free(void *p) {
  if (!p) return;
  uint64_t c = SD_HDR(p);
  sd_tcache *tc = sd_tcache_peek();
  /* c - 1 < SD_NCLASS - 1: a small block (c == 0 wraps around). */
  if (SD_LIKELY(tc && c - 1 < SD_NCLASS - 1)) {
    sd_flist *l = &tc->l[c];
    SD_NEXT(p) = l->head;
    l->head = p;
    if (SD_UNLIKELY(--l->room < 0)) sd_flush_batch(l, (int)c, sd_class_batch[c]);
    return;
  }
  sd_mem_free_slow(p, c);
}

size_t sd_mem_usable(const void *p, size_t n) {
  uint64_t c = SD_HDR(p);
  return c ? (size_t)sd_class_slot[c] - 8 : n;
}

void *sd_mem_realloc(void *p, size_t n) {
  if (!p) return sd_mem_alloc(n);
  uint64_t c = SD_HDR(p);
  if (SD_UNLIKELY(c >= SD_NCLASS)) sd_fatal("corrupt heap: bad block header");
  if (c == 0 && n > SD_SMALL_MAX) {
    char *b = realloc((char *)p - 16, n + 16);
    if (SD_UNLIKELY(!b)) sd_fatal("out of memory");
    return b + 16;
  }
  size_t have = c ? (size_t)sd_class_slot[c] - 8 : SD_SMALL_MAX + 1; /* large: at least */
  if (c && n <= have && n + 8 > (size_t)sd_class_slot[c - 1]) return p; /* same class */
  void *q = sd_mem_alloc(n);
  memcpy(q, p, n < have ? n : have);
  sd_mem_free(p);
  return q;
}
#endif

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

/* Freeing a box, list or map releases what it holds, which can free more of
 * them: dropping a long linked list (Node? next) or a deep tree would recurse
 * once per node and overflow the stack. So an object whose count drops to
 * zero while another one is being freed is queued, and the outermost call
 * frees the queue iteratively. The queue is threaded through the dead
 * objects' count fields (every counted object starts with int64_t rc), with
 * the kind in the low bits of the (8-byte aligned) pointer. */
/* The queue is per thread, and only used within one call (freeing never
 * parks), so it is only touched inside these noinline functions: a
 * goroutine moves between threads (runtime/README.md, "Thread-local
 * storage"). */
static _Thread_local int64_t sd_dead_head;
static _Thread_local bool sd_dead_busy;

/* After a crash unwound the thread's goroutine (possibly in the middle of a
 * free): forget the queue (what is still in it is leaked). */
SD_NOINLINE void sd_free_queue_reset_(void) {
  sd_dead_head = 0;
  sd_dead_busy = false;
}

static void sd_box_destroy(sd_box *b) {
  sd_release(b->t, sd_box_data(b));
  sd_obj_free(b);
}

/* Never inlined: the queue is thread-local, and a caller must not keep its
 * address across a park (runtime/README.md, "Thread-local storage"). Releasing never parks, so the
 * loop below runs on one thread. */
SD_NOINLINE void sd_free_queued_(void *obj, int kind) {
  /* An object that holds nothing counted frees nothing else: freed at once,
   * without the queue. */
  switch (kind) {
  case SD_DEAD_BOX:
    if (((sd_box *)obj)->t->trivial) {
      sd_obj_free(obj);
      return;
    }
    break;
  case SD_DEAD_LIST:
    if (((sd_list *)obj)->et->trivial) {
      sd_obj_free(obj);
      return;
    }
    break;
  default: {
    sd_map *m = obj;
    if (m->kt->trivial && m->vt->trivial) {
      sd_map_destroy_(m);
      return;
    }
  }
  }
  *(int64_t *)obj = sd_dead_head;
  sd_dead_head = (int64_t)((uintptr_t)obj | (uintptr_t)kind);
  if (sd_dead_busy) return;
  sd_dead_busy = true;
  while (sd_dead_head) {
    int64_t t = sd_dead_head;
    void *o = (void *)(uintptr_t)(t & ~(int64_t)7);
    sd_dead_head = *(int64_t *)o;
    switch ((int)(t & 7)) {
    case SD_DEAD_BOX: sd_box_destroy(o); break;
    case SD_DEAD_LIST: sd_list_destroy_(o); break;
    default: sd_map_destroy_(o); break;
    }
  }
  sd_dead_busy = false;
}

void sd_box_free_(sd_box *b) { sd_free_queued_(b, SD_DEAD_BOX); }

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
