/* Seed runtime. seed0 pastes this file at the top of every generated program
 * (the Makefile turns it into a C string, build/runtime.inc).
 *
 * Representation summary (see seed0/README.md):
 *   int int64_t, float double, bool bool, byte uint8_t,
 *   string sd_str (pointer + length, immutable, shared freely),
 *   error sd_err* (NULL = no error), []T sd_list*, map[K]V sd_map*.
 * Lists and maps carry a reference count used for copy-on-write. Memory is
 * never freed. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <spawn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

extern char **environ;

#define SD_FN static __attribute__((unused))
#define SD_INLINE static inline __attribute__((unused))
#define SD_NORET __attribute__((noreturn))
#define SD_UNLIKELY(x) __builtin_expect(!!(x), 0)

typedef struct { const char *p; int64_t len; } sd_str; /* p == NULL only for a null string? */
typedef struct sd_err { sd_str msg; } sd_err;
typedef struct sd_list { int64_t rc, len, cap; char *data; } sd_list;
typedef struct { bool has; int64_t v; } sd_O_i;
typedef struct { bool has; double v; } sd_O_f;
typedef struct { bool has; bool v; } sd_O_b;
typedef struct { bool has; uint8_t v; } sd_O_y;
typedef void (*sd_rtfn)(void *);
typedef bool (*sd_eqfn)(const void *, const void *);
typedef int (*sd_cmpfn)(const void *, const void *);

/* ---- crashes ---- */

SD_FN SD_NORET void sd_crash(const char *fmt, ...) {
  va_list ap;
  fflush(stdout);
  va_start(ap, fmt);
  fputs("crash: ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  fflush(stderr);
  exit(2);
}

SD_FN SD_NORET void sd_crash_index(int64_t i, int64_t len) {
  sd_crash("index out of range: %lld with length %lld", (long long)i, (long long)len);
}

/* ---- memory: a bump allocator, never freed. Returns zeroed memory. ---- */

static char *sd_heap, *sd_heap_end;

SD_FN void *sd_alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (n == 0) n = 16;
  if (SD_UNLIKELY((size_t)(sd_heap_end - sd_heap) < n)) {
    if (n >= 65536) {
      void *p = calloc(1, n);
      if (!p) sd_crash("out of memory");
      return p;
    }
    sd_heap = calloc(1, 1 << 20);
    if (!sd_heap) sd_crash("out of memory");
    sd_heap_end = sd_heap + (1 << 20);
  }
  void *p = sd_heap;
  sd_heap += n;
  return p;
}

SD_FN void *sd_xrealloc(void *p, size_t n) {
  p = realloc(p, n ? n : 1);
  if (!p) sd_crash("out of memory");
  return p;
}

/* ---- integers ---- */

SD_INLINE int64_t sd_add(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_add_overflow(a, b, &r)) sd_crash("integer overflow");
  return r;
}
SD_INLINE int64_t sd_sub(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_sub_overflow(a, b, &r)) sd_crash("integer overflow");
  return r;
}
SD_INLINE int64_t sd_mul(int64_t a, int64_t b) {
  int64_t r;
  if (__builtin_mul_overflow(a, b, &r)) sd_crash("integer overflow");
  return r;
}
SD_INLINE int64_t sd_neg(int64_t a) {
  if (a == INT64_MIN) sd_crash("integer overflow");
  return -a;
}
SD_INLINE int64_t sd_div(int64_t a, int64_t b) {
  if (b == 0) sd_crash("division by zero");
  if (a == INT64_MIN && b == -1) sd_crash("integer overflow");
  return a / b;
}
SD_INLINE int64_t sd_mod(int64_t a, int64_t b) {
  if (b == 0) sd_crash("division by zero");
  if (b == -1) return 0;
  return a % b;
}
SD_INLINE int64_t sd_shl(int64_t a, int64_t b) {
  if (b < 0) sd_crash("negative shift count");
  if (a == 0) return 0;
  if (b >= 64) sd_crash("integer overflow");
  int64_t r = (int64_t)((uint64_t)a << b);
  if ((r >> b) != a) sd_crash("integer overflow");
  return r;
}
SD_INLINE int64_t sd_shr(int64_t a, int64_t b) {
  if (b < 0) sd_crash("negative shift count");
  if (b >= 64) return a < 0 ? -1 : 0;
  return a >> b;
}
SD_INLINE uint8_t sd_shl_y(uint8_t a, uint8_t b) { return b >= 8 ? 0 : (uint8_t)(a << b); }
SD_INLINE uint8_t sd_shr_y(uint8_t a, uint8_t b) { return b >= 8 ? 0 : (uint8_t)(a >> b); }

SD_INLINE int64_t sd_f2i(double f) {
  if (isnan(f) || isinf(f) || f >= 9223372036854775808.0 || f < -9223372036854775808.0)
    sd_crash("cannot convert float %g to int", f);
  return (int64_t)f;
}
SD_INLINE uint8_t sd_i2y(int64_t i) {
  if (i < 0 || i > 255) sd_crash("cannot convert %lld to byte", (long long)i);
  return (uint8_t)i;
}

/* ---- strings ---- */

#define SD_S(lit) ((sd_str){lit, sizeof(lit) - 1})

SD_FN sd_str sd_str_alloc(int64_t n, char **out) {
  char *p = sd_alloc((size_t)n + 1);
  *out = p;
  return (sd_str){p, n};
}

SD_FN sd_str sd_str_copy(const char *s, int64_t n) {
  char *p;
  sd_str r = sd_str_alloc(n, &p);
  memcpy(p, s, (size_t)n);
  return r;
}

SD_FN char *sd_cstr(sd_str s) {
  char *p = sd_alloc((size_t)s.len + 1);
  memcpy(p, s.p, (size_t)s.len);
  return p;
}

SD_FN sd_str sd_cat(sd_str a, sd_str b) {
  if (a.len == 0) return b;
  if (b.len == 0) return a;
  char *p;
  sd_str r = sd_str_alloc(a.len + b.len, &p);
  memcpy(p, a.p, (size_t)a.len);
  memcpy(p + a.len, b.p, (size_t)b.len);
  return r;
}

SD_INLINE bool sd_str_eq(sd_str a, sd_str b) {
  return a.len == b.len && (a.p == b.p || memcmp(a.p, b.p, (size_t)a.len) == 0);
}

SD_FN int sd_str_cmp(sd_str a, sd_str b) {
  int64_t n = a.len < b.len ? a.len : b.len;
  int c = n ? memcmp(a.p, b.p, (size_t)n) : 0;
  if (c) return c < 0 ? -1 : 1;
  return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

SD_INLINE uint8_t sd_str_at(sd_str s, int64_t i) {
  if (SD_UNLIKELY((uint64_t)i >= (uint64_t)s.len)) sd_crash_index(i, s.len);
  return (uint8_t)s.p[i];
}

SD_FN sd_str sd_str_slice(sd_str s, int64_t a, int64_t b) {
  if (a < 0 || a > b || b > s.len)
    sd_crash("slice bounds out of range: [%lld:%lld] with length %lld", (long long)a, (long long)b,
             (long long)s.len);
  return (sd_str){s.p + a, b - a};
}

/* First index of sub in s at or after from, or -1. */
SD_FN int64_t sd_str_find(sd_str s, sd_str sub, int64_t from) {
  if (sub.len == 0) return from <= s.len ? from : -1;
  int64_t last = s.len - sub.len;
  char c0 = sub.p[0];
  for (int64_t i = from; i <= last; i++) {
    const char *q = memchr(s.p + i, c0, (size_t)(last - i + 1));
    if (!q) return -1;
    i = q - s.p;
    if (memcmp(q, sub.p, (size_t)sub.len) == 0) return i;
  }
  return -1;
}

SD_FN bool sd_str_contains(sd_str s, sd_str sub) { return sd_str_find(s, sub, 0) >= 0; }

SD_FN sd_O_i sd_str_index(sd_str s, sd_str sub) {
  int64_t i = sd_str_find(s, sub, 0);
  return i < 0 ? (sd_O_i){0} : (sd_O_i){true, i};
}

SD_FN bool sd_str_starts(sd_str s, sd_str p) {
  return p.len <= s.len && memcmp(s.p, p.p, (size_t)p.len) == 0;
}

SD_FN bool sd_str_ends(sd_str s, sd_str p) {
  return p.len <= s.len && memcmp(s.p + s.len - p.len, p.p, (size_t)p.len) == 0;
}

SD_FN sd_list *sd_list_new(int64_t esz, int64_t cap);
SD_FN void *sd_list_push(sd_list **pl, int64_t esz, sd_rtfn rt);

SD_FN sd_list *sd_str_split(sd_str s, sd_str sep) {
  if (sep.len == 0) sd_crash("split: empty separator");
  sd_list *l = sd_list_new(sizeof(sd_str), 4);
  int64_t start = 0;
  for (;;) {
    int64_t i = sd_str_find(s, sep, start);
    if (i < 0) break;
    *(sd_str *)sd_list_push(&l, sizeof(sd_str), NULL) = (sd_str){s.p + start, i - start};
    start = i + sep.len;
  }
  *(sd_str *)sd_list_push(&l, sizeof(sd_str), NULL) = (sd_str){s.p + start, s.len - start};
  return l;
}

SD_INLINE bool sd_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

SD_FN sd_str sd_str_trim(sd_str s) {
  int64_t a = 0, b = s.len;
  while (a < b && sd_is_space(s.p[a])) a++;
  while (b > a && sd_is_space(s.p[b - 1])) b--;
  return (sd_str){s.p + a, b - a};
}

SD_FN sd_str sd_str_replace(sd_str s, sd_str old, sd_str new_) {
  if (old.len == 0) sd_crash("replace: empty string to replace");
  int64_t count = 0, i = 0;
  while ((i = sd_str_find(s, old, i)) >= 0) { count++; i += old.len; }
  if (count == 0) return s;
  char *p;
  sd_str r = sd_str_alloc(s.len + count * (new_.len - old.len), &p);
  int64_t start = 0;
  while ((i = sd_str_find(s, old, start)) >= 0) {
    memcpy(p, s.p + start, (size_t)(i - start)); p += i - start;
    memcpy(p, new_.p, (size_t)new_.len); p += new_.len;
    start = i + old.len;
  }
  memcpy(p, s.p + start, (size_t)(s.len - start));
  return r;
}

SD_FN sd_str sd_str_upper(sd_str s) {
  char *p;
  sd_str r = sd_str_alloc(s.len, &p);
  for (int64_t i = 0; i < s.len; i++) {
    char c = s.p[i];
    p[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  return r;
}

SD_FN sd_str sd_str_lower(sd_str s) {
  char *p;
  sd_str r = sd_str_alloc(s.len, &p);
  for (int64_t i = 0; i < s.len; i++) {
    char c = s.p[i];
    p[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  return r;
}

SD_FN sd_err *sd_errf(const char *fmt, ...) {
  va_list ap;
  char tmp[256];
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  sd_err *e = sd_alloc(sizeof *e);
  if (n < (int)sizeof tmp) {
    e->msg = sd_str_copy(tmp, n);
  } else {
    char *p;
    e->msg = sd_str_alloc(n, &p);
    va_start(ap, fmt);
    vsnprintf(p, (size_t)n + 1, fmt, ap);
    va_end(ap);
  }
  return e;
}

SD_FN sd_err *sd_str_toint(sd_str s, int64_t *out) {
  int64_t i = 0;
  bool neg = false;
  uint64_t v = 0;
  *out = 0;
  if (s.len > 0 && s.p[0] == '-') { neg = true; i = 1; }
  if (i == s.len) goto bad;
  uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
  for (; i < s.len; i++) {
    char c = s.p[i];
    if (c < '0' || c > '9') goto bad;
    uint64_t d = (uint64_t)(c - '0');
    if (v > (limit - d) / 10) goto bad;
    v = v * 10 + d;
  }
  *out = neg ? (int64_t)(0 - v) : (int64_t)v;
  return NULL;
bad:
  return sd_errf("invalid integer: \"%.*s\"", (int)s.len, s.p);
}

SD_FN sd_err *sd_str_tofloat(sd_str s, double *out) {
  *out = 0;
  if (s.len == 0 || sd_is_space(s.p[0])) goto bad;
  char *c = sd_cstr(s), *end;
  errno = 0;
  double d = strtod(c, &end);
  if (end != c + s.len) goto bad;
  *out = d;
  return NULL;
bad:
  return sd_errf("invalid float: \"%.*s\"", (int)s.len, s.p);
}

SD_FN sd_list *sd_str_bytes(sd_str s) {
  sd_list *l = sd_list_new(1, s.len);
  memcpy(l->data, s.p, (size_t)s.len);
  l->len = s.len;
  return l;
}

SD_FN sd_str sd_str_from_bytes(sd_list *l) { return sd_str_copy(l->data, l->len); }

SD_FN sd_str sd_int_str(int64_t v) {
  char buf[32];
  int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
  return sd_str_copy(buf, n);
}

SD_FN sd_str sd_bool_str(bool b) { return b ? SD_S("true") : SD_S("false"); }

/* Shortest text that reads back as the same value (like Python's repr):
 * fixed notation when -4 <= exponent < 16, otherwise d.ddde+XX. */
SD_FN sd_str sd_float_str(double x) {
  if (isnan(x)) return SD_S("nan");
  if (isinf(x)) return x > 0 ? SD_S("inf") : SD_S("-inf");
  char buf[64], digits[32], out[64];
  int p;
  for (p = 1; p <= 17; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, x);
    if (strtod(buf, NULL) == x) break;
  }
  const char *q = buf;
  bool neg = false;
  if (*q == '-') { neg = true; q++; }
  int nd = 0;
  while (*q && *q != 'e') {
    if (*q >= '0' && *q <= '9') digits[nd++] = *q;
    q++;
  }
  int exp = atoi(q + 1);
  while (nd > 1 && digits[nd - 1] == '0') nd--;
  int n = 0;
  if (neg) out[n++] = '-';
  if (exp >= -4 && exp < 16) {
    if (exp >= 0) {
      for (int i = 0; i <= exp; i++) out[n++] = i < nd ? digits[i] : '0';
      out[n++] = '.';
      if (nd > exp + 1) {
        for (int i = exp + 1; i < nd; i++) out[n++] = digits[i];
      } else {
        out[n++] = '0';
      }
    } else {
      out[n++] = '0';
      out[n++] = '.';
      for (int i = 0; i < -exp - 1; i++) out[n++] = '0';
      for (int i = 0; i < nd; i++) out[n++] = digits[i];
    }
  } else {
    out[n++] = digits[0];
    if (nd > 1) {
      out[n++] = '.';
      for (int i = 1; i < nd; i++) out[n++] = digits[i];
    }
    n += snprintf(out + n, sizeof out - (size_t)n, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
  }
  return sd_str_copy(out, n);
}

/* ---- errors ---- */

SD_FN sd_err *sd_err_new(sd_str msg) {
  sd_err *e = sd_alloc(sizeof *e);
  e->msg = msg;
  return e;
}

SD_FN sd_str sd_err_message(sd_err *e) {
  if (!e) sd_crash("message() called on a null error");
  return e->msg;
}

SD_FN bool sd_err_eq(sd_err *a, sd_err *b) {
  if (!a || !b) return a == b;
  return a == b || sd_str_eq(a->msg, b->msg);
}

/* ---- lists ---- */

SD_FN sd_list *sd_list_new(int64_t esz, int64_t cap) {
  sd_list *l = sd_alloc(sizeof *l);
  l->rc = 1;
  l->cap = cap;
  l->data = cap ? sd_xrealloc(NULL, (size_t)(cap * esz)) : NULL;
  return l;
}

SD_INLINE sd_list *sd_rt_list(sd_list *l) {
  if (l) l->rc++;
  return l;
}
SD_INLINE void sd_rl_list(sd_list *l) {
  if (l && l->rc > 0) l->rc--;
}

SD_FN sd_list *sd_list_clone(sd_list *l, int64_t esz, sd_rtfn rt) {
  sd_list *n = sd_list_new(esz, l->len);
  if (l->len) memcpy(n->data, l->data, (size_t)(l->len * esz));
  n->len = l->len;
  if (rt)
    for (int64_t i = 0; i < n->len; i++) rt(n->data + i * esz);
  return n;
}

/* Make *pl safe to modify: copy it when it is shared. */
SD_INLINE sd_list *sd_list_mut(sd_list **pl, int64_t esz, sd_rtfn rt) {
  sd_list *l = *pl;
  if (SD_UNLIKELY(l->rc > 1)) {
    l->rc--;
    l = sd_list_clone(l, esz, rt);
    *pl = l;
  }
  return l;
}

SD_INLINE void *sd_list_at(sd_list *l, int64_t i, int64_t esz) {
  if (SD_UNLIKELY((uint64_t)i >= (uint64_t)l->len)) sd_crash_index(i, l->len);
  return l->data + i * esz;
}

SD_INLINE void *sd_list_at_mut(sd_list **pl, int64_t i, int64_t esz, sd_rtfn rt) {
  if (SD_UNLIKELY((uint64_t)i >= (uint64_t)(*pl)->len)) sd_crash_index(i, (*pl)->len);
  return sd_list_mut(pl, esz, rt)->data + i * esz;
}

SD_FN void sd_list_grow(sd_list *l, int64_t esz, int64_t need) {
  int64_t cap = l->cap ? l->cap * 2 : 4;
  while (cap < need) cap *= 2;
  l->data = sd_xrealloc(l->data, (size_t)(cap * esz));
  l->cap = cap;
}

SD_FN void *sd_list_push(sd_list **pl, int64_t esz, sd_rtfn rt) {
  sd_list *l = sd_list_mut(pl, esz, rt);
  if (l->len == l->cap) sd_list_grow(l, esz, l->len + 1);
  return l->data + (l->len++) * esz;
}

/* Returns a pointer to the removed last element, or NULL when empty. */
SD_FN void *sd_list_pop(sd_list **pl, int64_t esz, sd_rtfn rt) {
  if ((*pl)->len == 0) return NULL;
  sd_list *l = sd_list_mut(pl, esz, rt);
  l->len--;
  return l->data + l->len * esz;
}

SD_FN void *sd_list_insert(sd_list **pl, int64_t i, int64_t esz, sd_rtfn rt) {
  if (i < 0 || i > (*pl)->len) sd_crash("insert index out of range: %lld with length %lld", (long long)i, (long long)(*pl)->len);
  sd_list *l = sd_list_mut(pl, esz, rt);
  if (l->len == l->cap) sd_list_grow(l, esz, l->len + 1);
  memmove(l->data + (i + 1) * esz, l->data + i * esz, (size_t)((l->len - i) * esz));
  l->len++;
  return l->data + i * esz;
}

SD_FN void sd_list_remove(sd_list **pl, int64_t i, int64_t esz, sd_rtfn rt) {
  if ((uint64_t)i >= (uint64_t)(*pl)->len) sd_crash_index(i, (*pl)->len);
  sd_list *l = sd_list_mut(pl, esz, rt);
  memmove(l->data + i * esz, l->data + (i + 1) * esz, (size_t)((l->len - i - 1) * esz));
  l->len--;
}

SD_FN void sd_list_reverse(sd_list **pl, int64_t esz, sd_rtfn rt) {
  sd_list *l = sd_list_mut(pl, esz, rt);
  char tmp[256];
  char *t = esz <= 256 ? tmp : sd_alloc((size_t)esz);
  for (int64_t i = 0, j = l->len - 1; i < j; i++, j--) {
    memcpy(t, l->data + i * esz, (size_t)esz);
    memcpy(l->data + i * esz, l->data + j * esz, (size_t)esz);
    memcpy(l->data + j * esz, t, (size_t)esz);
  }
}

SD_FN sd_list *sd_list_slice(sd_list *l, int64_t a, int64_t b, int64_t esz, sd_rtfn rt) {
  if (a < 0 || a > b || b > l->len)
    sd_crash("slice bounds out of range: [%lld:%lld] with length %lld", (long long)a, (long long)b,
             (long long)l->len);
  sd_list *n = sd_list_new(esz, b - a);
  if (b > a) memcpy(n->data, l->data + a * esz, (size_t)((b - a) * esz));
  n->len = b - a;
  if (rt)
    for (int64_t i = 0; i < n->len; i++) rt(n->data + i * esz);
  return n;
}

SD_FN int64_t sd_list_find(sd_list *l, const void *x, int64_t esz, sd_eqfn eq) {
  for (int64_t i = 0; i < l->len; i++)
    if (eq(l->data + i * esz, x)) return i;
  return -1;
}

SD_FN bool sd_list_eq(sd_list *a, sd_list *b, int64_t esz, sd_eqfn eq) {
  if (a == b) return true;
  if (!a || !b || a->len != b->len) return false;
  for (int64_t i = 0; i < a->len; i++)
    if (!eq(a->data + i * esz, b->data + i * esz)) return false;
  return true;
}

/* Stable merge sort. */
SD_FN void sd_msort(char *a, char *tmp, int64_t n, int64_t esz, sd_cmpfn cmp) {
  if (n < 2) return;
  if (n <= 8) {
    for (int64_t i = 1; i < n; i++) {
      memcpy(tmp, a + i * esz, (size_t)esz);
      int64_t j = i;
      while (j > 0 && cmp(tmp, a + (j - 1) * esz) < 0) {
        memcpy(a + j * esz, a + (j - 1) * esz, (size_t)esz);
        j--;
      }
      memcpy(a + j * esz, tmp, (size_t)esz);
    }
    return;
  }
  int64_t m = n / 2;
  sd_msort(a, tmp, m, esz, cmp);
  sd_msort(a + m * esz, tmp, n - m, esz, cmp);
  if (cmp(a + m * esz, a + (m - 1) * esz) >= 0) return;
  int64_t i = 0, j = m, k = 0;
  while (i < m && j < n) {
    if (cmp(a + j * esz, a + i * esz) < 0) memcpy(tmp + (k++) * esz, a + (j++) * esz, (size_t)esz);
    else memcpy(tmp + (k++) * esz, a + (i++) * esz, (size_t)esz);
  }
  while (i < m) memcpy(tmp + (k++) * esz, a + (i++) * esz, (size_t)esz);
  while (j < n) memcpy(tmp + (k++) * esz, a + (j++) * esz, (size_t)esz);
  memcpy(a, tmp, (size_t)(n * esz));
}

SD_FN void sd_list_sort(sd_list **pl, int64_t esz, sd_rtfn rt, sd_cmpfn cmp) {
  sd_list *l = sd_list_mut(pl, esz, rt);
  if (l->len < 2) return;
  char *tmp = sd_xrealloc(NULL, (size_t)(l->len * esz));
  sd_msort(l->data, tmp, l->len, esz, cmp);
  free(tmp);
}

SD_FN int sd_cmp_i(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return (x > y) - (x < y);
}
SD_FN int sd_cmp_f(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}
SD_FN int sd_cmp_y(const void *a, const void *b) {
  uint8_t x = *(const uint8_t *)a, y = *(const uint8_t *)b;
  return (x > y) - (x < y);
}
SD_FN int sd_cmp_s(const void *a, const void *b) { return sd_str_cmp(*(const sd_str *)a, *(const sd_str *)b); }

SD_FN sd_str sd_list_join(sd_list *l, sd_str sep) {
  sd_str *xs = (sd_str *)l->data;
  if (l->len == 0) return SD_S("");
  if (l->len == 1) return xs[0];
  int64_t n = sep.len * (l->len - 1);
  for (int64_t i = 0; i < l->len; i++) n += xs[i].len;
  char *p;
  sd_str r = sd_str_alloc(n, &p);
  for (int64_t i = 0; i < l->len; i++) {
    if (i) { memcpy(p, sep.p, (size_t)sep.len); p += sep.len; }
    memcpy(p, xs[i].p, (size_t)xs[i].len);
    p += xs[i].len;
  }
  return r;
}

/* ---- maps: insertion-ordered hash maps ---- */

typedef struct { uint64_t h; int64_t ik; sd_str sk; int64_t live; } sd_ment;
typedef struct sd_map {
  int64_t rc, len, n, cap, vsz, icap;
  int skey;          /* 1: string keys, 0: int/byte/bool keys stored as int64 */
  sd_ment *e;        /* entries in insertion order, cap of them, n used */
  char *v;           /* values, parallel to e */
  int32_t *ix;       /* open-addressing index: 0 empty, -1 deleted, k+1 entry k */
} sd_map;

SD_INLINE uint64_t sd_hash_i(int64_t k) {
  uint64_t x = (uint64_t)k + 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
SD_INLINE uint64_t sd_hash_s(sd_str s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int64_t i = 0; i < s.len; i++) h = (h ^ (uint8_t)s.p[i]) * 0x100000001b3ULL;
  return h ^ (h >> 29);
}

SD_FN sd_map *sd_map_new(int skey, int64_t vsz) {
  sd_map *m = sd_alloc(sizeof *m);
  m->rc = 1;
  m->skey = skey;
  m->vsz = vsz;
  return m;
}

SD_INLINE sd_map *sd_rt_map(sd_map *m) {
  if (m) m->rc++;
  return m;
}
SD_INLINE void sd_rl_map(sd_map *m) {
  if (m && m->rc > 0) m->rc--;
}

/* Index slot holding the key, or -1. */
SD_FN int64_t sd_map_probe(sd_map *m, uint64_t h, int64_t ik, sd_str sk) {
  if (!m->icap) return -1;
  uint64_t mask = (uint64_t)m->icap - 1, i = h & mask;
  for (;;) {
    int32_t x = m->ix[i];
    if (x == 0) return -1;
    if (x > 0) {
      sd_ment *e = &m->e[x - 1];
      if (e->h == h && (m->skey ? sd_str_eq(e->sk, sk) : e->ik == ik)) return (int64_t)i;
    }
    i = (i + 1) & mask;
  }
}

SD_FN void sd_map_index_add(sd_map *m, int64_t k) {
  uint64_t mask = (uint64_t)m->icap - 1, i = m->e[k].h & mask;
  while (m->ix[i] > 0) i = (i + 1) & mask;
  m->ix[i] = (int32_t)(k + 1);
}

/* Compact live entries into arrays of capacity ncap and rebuild the index. */
SD_FN void sd_map_rebuild(sd_map *m, int64_t ncap) {
  sd_ment *ne = sd_xrealloc(NULL, (size_t)ncap * sizeof(sd_ment));
  char *nv = sd_xrealloc(NULL, (size_t)(ncap * m->vsz));
  int64_t j = 0;
  for (int64_t i = 0; i < m->n; i++) {
    if (!m->e[i].live) continue;
    ne[j] = m->e[i];
    memcpy(nv + j * m->vsz, m->v + i * m->vsz, (size_t)m->vsz);
    j++;
  }
  free(m->e);
  free(m->v);
  free(m->ix);
  m->e = ne;
  m->v = nv;
  m->n = j;
  m->cap = ncap;
  m->icap = ncap * 2;
  m->ix = calloc((size_t)m->icap, sizeof(int32_t));
  if (!m->ix) sd_crash("out of memory");
  for (int64_t i = 0; i < j; i++) sd_map_index_add(m, i);
}

SD_FN sd_map *sd_map_clone(sd_map *m, sd_rtfn rt) {
  sd_map *c = sd_map_new(m->skey, m->vsz);
  if (m->n) {
    m->rc++; /* keep m alive and unchanged while copying */
    c->len = m->len;
    c->n = m->n;
    c->cap = m->cap;
    c->icap = m->icap;
    c->e = sd_xrealloc(NULL, (size_t)m->cap * sizeof(sd_ment));
    c->v = sd_xrealloc(NULL, (size_t)(m->cap * m->vsz));
    c->ix = sd_xrealloc(NULL, (size_t)m->icap * sizeof(int32_t));
    memcpy(c->e, m->e, (size_t)m->n * sizeof(sd_ment));
    memcpy(c->v, m->v, (size_t)(m->n * m->vsz));
    memcpy(c->ix, m->ix, (size_t)m->icap * sizeof(int32_t));
    m->rc--;
    if (rt)
      for (int64_t i = 0; i < c->n; i++)
        if (c->e[i].live) rt(c->v + i * c->vsz);
  }
  return c;
}

SD_INLINE sd_map *sd_map_mut(sd_map **pm, sd_rtfn rt) {
  sd_map *m = *pm;
  if (SD_UNLIKELY(m->rc > 1)) {
    m->rc--;
    m = sd_map_clone(m, rt);
    *pm = m;
  }
  return m;
}

SD_FN void *sd_map_get(sd_map *m, uint64_t h, int64_t ik, sd_str sk) {
  int64_t s = sd_map_probe(m, h, ik, sk);
  return s < 0 ? NULL : m->v + (m->ix[s] - 1) * m->vsz;
}

/* Pointer to the value for the key, inserting a zeroed value if it is new. */
SD_FN void *sd_map_slot(sd_map **pm, uint64_t h, int64_t ik, sd_str sk, sd_rtfn rt) {
  sd_map *m = sd_map_mut(pm, rt);
  int64_t s = sd_map_probe(m, h, ik, sk);
  if (s >= 0) return m->v + (m->ix[s] - 1) * m->vsz;
  if (m->n == m->cap) sd_map_rebuild(m, m->len * 2 >= m->cap ? (m->cap ? m->cap * 2 : 8) : m->cap);
  int64_t k = m->n++;
  sd_ment *e = &m->e[k];
  e->h = h;
  e->ik = ik;
  e->sk = sk;
  e->live = 1;
  m->len++;
  memset(m->v + k * m->vsz, 0, (size_t)m->vsz);
  sd_map_index_add(m, k);
  return m->v + k * m->vsz;
}

SD_FN void sd_map_del(sd_map **pm, uint64_t h, int64_t ik, sd_str sk, sd_rtfn rt) {
  if (sd_map_probe(*pm, h, ik, sk) < 0) return;
  sd_map *m = sd_map_mut(pm, rt);
  int64_t s = sd_map_probe(m, h, ik, sk);
  m->e[m->ix[s] - 1].live = 0;
  m->ix[s] = -1;
  m->len--;
}

SD_INLINE void *sd_map_get_i(sd_map *m, int64_t k) { return sd_map_get(m, sd_hash_i(k), k, (sd_str){0}); }
SD_INLINE void *sd_map_get_s(sd_map *m, sd_str k) { return sd_map_get(m, sd_hash_s(k), 0, k); }
SD_INLINE void *sd_map_slot_i(sd_map **pm, int64_t k, sd_rtfn rt) {
  return sd_map_slot(pm, sd_hash_i(k), k, (sd_str){0}, rt);
}
SD_INLINE void *sd_map_slot_s(sd_map **pm, sd_str k, sd_rtfn rt) {
  return sd_map_slot(pm, sd_hash_s(k), 0, k, rt);
}
SD_INLINE void sd_map_del_i(sd_map **pm, int64_t k, sd_rtfn rt) { sd_map_del(pm, sd_hash_i(k), k, (sd_str){0}, rt); }
SD_INLINE void sd_map_del_s(sd_map **pm, sd_str k, sd_rtfn rt) { sd_map_del(pm, sd_hash_s(k), 0, k, rt); }

/* kt: 0 int, 1 byte, 2 bool, 3 string */
SD_FN sd_list *sd_map_keys(sd_map *m, int kt) {
  int64_t esz = kt == 0 ? 8 : kt == 1 ? 1 : kt == 2 ? (int64_t)sizeof(bool) : (int64_t)sizeof(sd_str);
  sd_list *l = sd_list_new(esz, m->len);
  for (int64_t i = 0; i < m->n; i++) {
    sd_ment *e = &m->e[i];
    if (!e->live) continue;
    char *d = l->data + (l->len++) * esz;
    switch (kt) {
    case 0: *(int64_t *)d = e->ik; break;
    case 1: *(uint8_t *)d = (uint8_t)e->ik; break;
    case 2: *(bool *)d = e->ik != 0; break;
    default: *(sd_str *)d = e->sk; break;
    }
  }
  return l;
}

/* Maps are equal when they hold the same keys with equal values (order is ignored). */
SD_FN bool sd_map_eq(sd_map *a, sd_map *b, sd_eqfn eq) {
  if (a == b) return true;
  if (!a || !b || a->len != b->len) return false;
  for (int64_t i = 0; i < a->n; i++) {
    sd_ment *e = &a->e[i];
    if (!e->live) continue;
    void *bv = sd_map_get(b, e->h, e->ik, e->sk);
    if (!bv || !eq(a->v + i * a->vsz, bv)) return false;
  }
  return true;
}

/* ---- fmt ---- */

SD_FN void sd_print(sd_str s) { fwrite(s.p, 1, (size_t)s.len, stdout); }
SD_FN void sd_println(sd_str s) {
  fwrite(s.p, 1, (size_t)s.len, stdout);
  putc('\n', stdout);
}
SD_FN void sd_eprint(sd_str s) {
  fflush(stdout);
  fwrite(s.p, 1, (size_t)s.len, stderr);
}
SD_FN void sd_eprintln(sd_str s) {
  fflush(stdout);
  fwrite(s.p, 1, (size_t)s.len, stderr);
  putc('\n', stderr);
}

/* ---- os ---- */

SD_FN SD_NORET void sd_os_exit(int64_t code) {
  fflush(stdout);
  fflush(stderr);
  exit((int)code);
}

SD_FN sd_str sd_os_env(sd_str name) {
  const char *v = getenv(sd_cstr(name));
  if (!v) return (sd_str){0};
  return sd_str_copy(v, (int64_t)strlen(v));
}

SD_FN sd_err *sd_os_run(sd_list *argv, int64_t *code) {
  *code = 0;
  if (argv->len == 0) return sd_errf("os.run: empty argument list");
  char **av = sd_alloc((size_t)(argv->len + 1) * sizeof(char *));
  for (int64_t i = 0; i < argv->len; i++) av[i] = sd_cstr(((sd_str *)argv->data)[i]);
  fflush(stdout);
  fflush(stderr);
  pid_t pid;
  int r = posix_spawnp(&pid, av[0], NULL, NULL, av, environ);
  if (r != 0) return sd_errf("%s: %s", av[0], strerror(r));
  int st;
  while (waitpid(pid, &st, 0) < 0)
    if (errno != EINTR) return sd_errf("%s: %s", av[0], strerror(errno));
  *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
  return NULL;
}

/* ---- fs ---- */

SD_FN sd_err *sd_fs_read(sd_str path, sd_str *out) {
  char *cp = sd_cstr(path);
  *out = (sd_str){0};
  fflush(stdout);
  FILE *f = fopen(cp, "rb");
  if (!f) return sd_errf("%s: %s", cp, strerror(errno));
  int64_t cap = 4096, n = 0;
  char *buf = sd_xrealloc(NULL, (size_t)cap);
  for (;;) {
    if (n == cap) { cap *= 2; buf = sd_xrealloc(buf, (size_t)cap); }
    size_t r = fread(buf + n, 1, (size_t)(cap - n), f);
    n += (int64_t)r;
    if (r == 0) break;
  }
  int bad = ferror(f);
  fclose(f);
  if (bad) { free(buf); return sd_errf("%s: read error", cp); }
  *out = sd_str_copy(buf, n);
  free(buf);
  return NULL;
}

SD_FN sd_err *sd_fs_write(sd_str path, sd_str data) {
  char *cp = sd_cstr(path);
  FILE *f = fopen(cp, "wb");
  if (!f) return sd_errf("%s: %s", cp, strerror(errno));
  size_t w = fwrite(data.p, 1, (size_t)data.len, f);
  int bad = fclose(f) != 0 || w != (size_t)data.len;
  if (bad) return sd_errf("%s: write error", cp);
  return NULL;
}

SD_FN bool sd_fs_exists(sd_str path) {
  struct stat st;
  return stat(sd_cstr(path), &st) == 0;
}

SD_FN bool sd_fs_isdir(sd_str path) {
  struct stat st;
  return stat(sd_cstr(path), &st) == 0 && S_ISDIR(st.st_mode);
}

SD_FN sd_err *sd_fs_list(sd_str path, sd_list **out) {
  char *cp = sd_cstr(path);
  *out = NULL;
  DIR *d = opendir(cp);
  if (!d) return sd_errf("%s: %s", cp, strerror(errno));
  sd_list *l = sd_list_new(sizeof(sd_str), 8);
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    *(sd_str *)sd_list_push(&l, sizeof(sd_str), NULL) = sd_str_copy(de->d_name, (int64_t)strlen(de->d_name));
  }
  closedir(d);
  sd_list_sort(&l, sizeof(sd_str), NULL, sd_cmp_s);
  *out = l;
  return NULL;
}

SD_FN sd_err *sd_fs_mkdir(sd_str path) {
  char *cp = sd_cstr(path);
  size_t n = strlen(cp);
  for (size_t i = 1; i <= n; i++) {
    if (i < n && cp[i] != '/') continue;
    char c = cp[i];
    cp[i] = 0;
    if (mkdir(cp, 0777) != 0 && errno != EEXIST) {
      int e = errno;
      cp[i] = c;
      return sd_errf("%s: %s", cp, strerror(e));
    }
    cp[i] = c;
  }
  struct stat st;
  if (stat(cp, &st) != 0 || !S_ISDIR(st.st_mode)) return sd_errf("%s: not a directory", cp);
  return NULL;
}

SD_FN sd_err *sd_fs_remove(sd_str path) {
  char *cp = sd_cstr(path);
  if (remove(cp) != 0) return sd_errf("%s: %s", cp, strerror(errno));
  return NULL;
}

/* ---- program entry ---- */

static char sd_outbuf[1 << 16];
static char sd_altstack[1 << 16];

/* Stack overflow (deep recursion) or another memory fault: report it like
 * any other crash instead of dying silently with unflushed output. */
static void sd_on_fault(int sig) {
  static const char msg[] = "crash: stack overflow or invalid memory access\n";
  (void)sig;
  fflush(stdout);
  if (write(2, msg, sizeof msg - 1) < 0) _exit(2);
  _exit(2);
}

SD_FN sd_list *sd_start(int argc, char **argv) {
  setvbuf(stdout, sd_outbuf, _IOFBF, sizeof sd_outbuf);
  stack_t ss;
  ss.ss_sp = sd_altstack;
  ss.ss_size = sizeof sd_altstack;
  ss.ss_flags = 0;
  if (sigaltstack(&ss, NULL) == 0) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sd_on_fault;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
  }
  sd_list *l = sd_list_new(sizeof(sd_str), argc);
  for (int i = 0; i < argc; i++) ((sd_str *)l->data)[i] = sd_str_copy(argv[i], (int64_t)strlen(argv[i]));
  l->len = argc;
  return l;
}

SD_FN int sd_finish(sd_err *e) {
  fflush(stdout);
  if (e) {
    fprintf(stderr, "error: %.*s\n", (int)e->msg.len, e->msg.p);
    return 1;
  }
  return 0;
}
