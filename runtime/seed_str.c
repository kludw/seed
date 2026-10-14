/* Strings: immutable, counted, NUL-terminated byte strings. */
#include "seed_internal.h"

#include <math.h>

bool sd_int_parse_dec_(const char *p, int64_t len, sd_int *out);

/* Immortal one-byte strings and the empty string, shared by everyone. */
typedef struct {
  int64_t rc, len, cap;
  char data[2];
} sd_bytestr_t;
#define SD_B1(i) {SD_RC_IMMORTAL, 1, 0, {(char)(i), 0}}
#define SD_B4(i) SD_B1(i), SD_B1(i + 1), SD_B1(i + 2), SD_B1(i + 3)
#define SD_B16(i) SD_B4(i), SD_B4(i + 4), SD_B4(i + 8), SD_B4(i + 12)
#define SD_B64(i) SD_B16(i), SD_B16(i + 16), SD_B16(i + 32), SD_B16(i + 48)
static const sd_bytestr_t sd_bytestr_[256] = {SD_B64(0), SD_B64(64), SD_B64(128), SD_B64(192)};
static const struct {
  int64_t rc, len, cap;
  char data[1];
} sd_empty_ = {SD_RC_IMMORTAL, 0, 0, ""};

sd_str sd_str_byte_(uint8_t c) { return (sd_str)(void *)&sd_bytestr_[c]; }

sd_str sd_str_empty(void) { return (sd_str)(void *)&sd_empty_; }

sd_str sd_str_alloc_(int64_t len) {
  if (len == 0) return sd_str_empty();
  sd_str s = sd_obj_alloc(sizeof(sd_string) + (size_t)len + 1);
  s->rc = 1;
  s->len = len;
  s->cap = len;
  s->data[len] = 0;
  return s;
}

void sd_str_free_(sd_str s) { sd_obj_free(s); }

sd_str sd_str_new(const char *p, int64_t n) {
  if (n == 1) return sd_str_byte_((uint8_t)p[0]);
  sd_str s = sd_str_alloc_(n);
  if (n) memcpy(s->data, p, (size_t)n);
  return s;
}

sd_str sd_str_from_cstr(const char *s) { return sd_str_new(s, (int64_t)strlen(s)); }

sd_str sd_str_concat(sd_str a, sd_str b) {
  if (a->len == 0) return sd_str_retain(b);
  if (b->len == 0) return sd_str_retain(a);
  sd_str r = sd_str_alloc_(a->len + b->len);
  memcpy(r->data, a->data, (size_t)a->len);
  memcpy(r->data + a->len, b->data, (size_t)b->len);
  return r;
}

sd_str sd_str_concat_n(const sd_str *parts, int n) {
  int64_t len = 0;
  for (int i = 0; i < n; i++) len += parts[i]->len;
  if (len == 0) return sd_str_empty();
  sd_str r = sd_str_alloc_(len);
  char *p = r->data;
  for (int i = 0; i < n; i++) {
    memcpy(p, parts[i]->data, (size_t)parts[i]->len);
    p += parts[i]->len;
  }
  return r;
}

/* The number of characters of the decimal text of v. */
static int dec_len(int64_t v) {
  uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  int n = v < 0 ? 2 : 1;
  while (m >= 10) {
    m /= 10;
    n++;
  }
  return n;
}

/* Writes the n characters of the decimal text of v at p. */
static void dec_write(char *p, int n, int64_t v) {
  uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  char *q = p + n;
  do {
    *--q = (char)('0' + (int)(m % 10));
    m /= 10;
  } while (m);
  if (v < 0) *--q = '-';
}

sd_str sd_str_concat_parts(const sd_str_part *parts, int n) {
  sd_str local[8];
  sd_str *big = n <= 8 ? local : sd_alloc((size_t)n * sizeof *big); /* text of big ints */
  int64_t len = 0;
  for (int i = 0; i < n; i++) {
    big[i] = NULL;
    if (parts[i].s) {
      len += parts[i].s->len;
    } else if (sd_int_is_small(parts[i].i)) {
      len += dec_len(sd_int_small_value(parts[i].i));
    } else {
      big[i] = sd_int_to_str(parts[i].i);
      len += big[i]->len;
    }
  }
  sd_str r = sd_str_alloc_(len);
  char *p = r->data;
  for (int i = 0; i < n; i++) {
    sd_str s = parts[i].s ? parts[i].s : big[i];
    if (s) {
      memcpy(p, s->data, (size_t)s->len);
      p += s->len;
      if (big[i]) sd_str_release(big[i]);
    } else {
      int64_t v = sd_int_small_value(parts[i].i);
      int k = dec_len(v);
      dec_write(p, k, v);
      p += k;
    }
  }
  if (big != local) free(big);
  return r;
}

sd_str sd_str_append(sd_str a, sd_str b) {
  if (b->len == 0) return a;
  if (a->rc == 1) {
    int64_t need = a->len + b->len;
    if (need > a->cap) {
      int64_t cap = a->cap * 2;
      if (cap < need) cap = need;
      if (cap < 16) cap = 16;
      a = sd_realloc(a, sizeof(sd_string) + (size_t)cap + 1);
      a->cap = cap;
    }
    memcpy(a->data + a->len, b->data, (size_t)b->len);
    a->len = need;
    a->data[need] = 0;
    return a;
  }
  sd_str r = sd_str_concat(a, b);
  sd_str_release(a);
  return r;
}

bool sd_str_eq(sd_str a, sd_str b) {
  if (a == b) return true;
  if (!a || !b) return false;
  return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

int sd_str_cmp(sd_str a, sd_str b) {
  int64_t n = a->len < b->len ? a->len : b->len;
  int c = n ? memcmp(a->data, b->data, (size_t)n) : 0;
  if (c) return c < 0 ? -1 : 1;
  return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

uint64_t sd_str_hash(sd_str s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (int64_t i = 0; i < s->len; i++) h = (h ^ (uint8_t)s->data[i]) * 0x100000001b3ULL;
  return h ^ (h >> 29);
}

static SD_NORETURN void sd_crash_slice(sd_int a, sd_int b, int64_t len) {
  char *x = sd_int_to_cstr(a), *y = sd_int_to_cstr(b);
  char *m = sd_fmt("slice bounds out of range: [%s:%s] with length %lld", x, y, (long long)len);
  free(x);
  free(y);
  sd_crash_owned(m);
}

/* Checks 0 <= a <= b <= len. */
static void sd_slice_bounds(sd_int a, sd_int b, int64_t len, int64_t *pa, int64_t *pb) {
  int64_t x, y;
  if (!sd_int_to_i64(a, &x) || !sd_int_to_i64(b, &y) || x < 0 || x > y || y > len) sd_crash_slice(a, b, len);
  *pa = x;
  *pb = y;
}

sd_str sd_str_slice(sd_str s, sd_int a, sd_int b) {
  int64_t x, y;
  sd_slice_bounds(a, b, s->len, &x, &y);
  if (x == 0 && y == s->len) return sd_str_retain(s);
  return sd_str_new(s->data + x, y - x);
}

/* First index of sub in s at or after from, or -1. */
static int64_t sd_str_find(sd_str s, sd_str sub, int64_t from) {
  if (sub->len == 0) return from <= s->len ? from : -1;
  int64_t last = s->len - sub->len;
  char c0 = sub->data[0];
  for (int64_t i = from; i <= last; i++) {
    const char *q = memchr(s->data + i, c0, (size_t)(last - i + 1));
    if (!q) return -1;
    i = q - s->data;
    if (memcmp(q, sub->data, (size_t)sub->len) == 0) return i;
  }
  return -1;
}

bool sd_str_contains(sd_str s, sd_str sub) { return sd_str_find(s, sub, 0) >= 0; }

sd_int sd_str_index(sd_str s, sd_str sub) {
  int64_t i = sd_str_find(s, sub, 0);
  return i < 0 ? SD_INT_NULL : SD_INT(i);
}

bool sd_str_starts_with(sd_str s, sd_str p) {
  return p->len <= s->len && memcmp(s->data, p->data, (size_t)p->len) == 0;
}

bool sd_str_ends_with(sd_str s, sd_str p) {
  return p->len <= s->len && memcmp(s->data + s->len - p->len, p->data, (size_t)p->len) == 0;
}

sd_list *sd_str_split(sd_str s, sd_str sep) {
  if (sep->len == 0) sd_crash("split: empty separator");
  sd_list *l = sd_list_new(&sd_type_str, 4);
  int64_t start = 0;
  for (;;) {
    int64_t i = sd_str_find(s, sep, start);
    if (i < 0) break;
    SD_LIST_PUSH(&l, sd_str, sd_str_new(s->data + start, i - start));
    start = i + sep->len;
  }
  SD_LIST_PUSH(&l, sd_str, start == 0 ? sd_str_retain(s) : sd_str_new(s->data + start, s->len - start));
  return l;
}

static bool sd_is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

sd_str sd_str_trim(sd_str s) {
  int64_t a = 0, b = s->len;
  while (a < b && sd_is_space(s->data[a])) a++;
  while (b > a && sd_is_space(s->data[b - 1])) b--;
  if (a == 0 && b == s->len) return sd_str_retain(s);
  return sd_str_new(s->data + a, b - a);
}

sd_str sd_str_replace(sd_str s, sd_str old, sd_str new_) {
  if (old->len == 0) sd_crash("replace: empty string to replace");
  int64_t count = 0, i = 0;
  while ((i = sd_str_find(s, old, i)) >= 0) {
    count++;
    i += old->len;
  }
  if (count == 0) return sd_str_retain(s);
  int64_t len = s->len + count * (new_->len - old->len);
  if (len == 0) return sd_str_empty();
  sd_str r = sd_str_alloc_(len);
  char *p = r->data;
  int64_t start = 0;
  while ((i = sd_str_find(s, old, start)) >= 0) {
    memcpy(p, s->data + start, (size_t)(i - start));
    p += i - start;
    memcpy(p, new_->data, (size_t)new_->len);
    p += new_->len;
    start = i + old->len;
  }
  memcpy(p, s->data + start, (size_t)(s->len - start));
  return r;
}

static sd_str sd_str_map_ascii(sd_str s, char lo, char hi, int delta) {
  int64_t i = 0;
  while (i < s->len && !(s->data[i] >= lo && s->data[i] <= hi)) i++;
  if (i == s->len) return sd_str_retain(s);
  sd_str r = sd_str_alloc_(s->len);
  memcpy(r->data, s->data, (size_t)s->len);
  for (; i < s->len; i++) {
    char c = r->data[i];
    if (c >= lo && c <= hi) r->data[i] = (char)(c + delta);
  }
  if (r->len == 1) { /* keep one-byte strings canonical (immortal) */
    sd_str b = sd_str_byte_((uint8_t)r->data[0]);
    sd_str_release(r);
    return b;
  }
  return r;
}

sd_str sd_str_to_upper(sd_str s) { return sd_str_map_ascii(s, 'a', 'z', 'A' - 'a'); }
sd_str sd_str_to_lower(sd_str s) { return sd_str_map_ascii(s, 'A', 'Z', 'a' - 'A'); }

sd_err sd_str_to_int(sd_str s, sd_int *out) {
  if (sd_int_parse_dec_(s->data, s->len, out)) return SD_ERR_NULL;
  *out = SD_INT_NULL;
  return sd_errorf("string.toInt: \"%s\": invalid integer", s->data);
}

sd_err sd_str_to_float(sd_str s, double *out) {
  *out = 0;
  if (s->len == 0 || sd_is_space(s->data[0]) || (int64_t)strlen(s->data) != s->len) goto bad;
  char *end;
  double d = strtod(s->data, &end);
  if (end != s->data + s->len) goto bad;
  *out = d;
  return SD_ERR_NULL;
bad:
  return sd_errorf("string.toFloat: \"%s\": invalid float", s->data);
}

sd_list *sd_str_bytes(sd_str s) {
  sd_list *l = sd_list_new_len_(&sd_type_byte, s->len);
  if (s->len) memcpy(sd_list_data(l), s->data, (size_t)s->len);
  return l;
}

/* Length of the UTF-8 sequence starting at p (at most n bytes), or 1 for an
 * invalid or truncated sequence (each such byte becomes its own string). */
static int64_t sd_utf8_len(const unsigned char *p, int64_t n) {
  unsigned char c = p[0];
  int64_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
  if (k == 0 || k > n) return 1;
  for (int64_t i = 1; i < k; i++)
    if ((p[i] & 0xC0) != 0x80) return 1;
  return k;
}

sd_list *sd_str_chars(sd_str s) {
  sd_list *l = sd_list_new(&sd_type_str, s->len);
  const unsigned char *p = (const unsigned char *)s->data;
  for (int64_t i = 0; i < s->len;) {
    int64_t k = sd_utf8_len(p + i, s->len - i);
    SD_LIST_PUSH(&l, sd_str, sd_str_new(s->data + i, k));
    i += k;
  }
  return l;
}

sd_str sd_str_from_bytes(const sd_list *bytes) { return sd_str_new(sd_list_data(bytes), bytes->len); }

/* ------------------------------------------------------------ to string */

sd_str sd_int_to_str(sd_int a) {
  if ((a.w & 1) == 0) {
    char buf[24];
    char *end = buf + sizeof buf, *p = end;
    int64_t v = a.w >> 1;
    uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
      *--p = (char)('0' + (int)(m % 10));
      m /= 10;
    } while (m);
    if (v < 0) *--p = '-';
    return sd_str_new(p, end - p);
  }
  char *c = sd_int_to_cstr(a);
  sd_str s = sd_str_from_cstr(c);
  free(c);
  return s;
}

sd_str sd_bool_to_str(bool b) { return b ? SD_STR_LIT("true") : SD_STR_LIT("false"); }

sd_str sd_byte_to_str(uint8_t b) {
  char buf[8];
  int n = snprintf(buf, sizeof buf, "%u", (unsigned)b);
  return sd_str_new(buf, n);
}

/* The shortest text that reads back as the same value (like Python's repr):
 * fixed notation when -4 <= exponent < 16, otherwise d.ddde+XX. Identical to
 * seed0's sd_float_str. */
sd_str sd_float_to_str(double x) {
  if (isnan(x)) return SD_STR_LIT("nan");
  if (isinf(x)) return x > 0 ? SD_STR_LIT("inf") : SD_STR_LIT("-inf");
  char buf[64], digits[32], out[64];
  int p;
  for (p = 1; p <= 17; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, x);
    if (strtod(buf, NULL) == x) break;
  }
  const char *q = buf;
  bool neg = false;
  if (*q == '-') {
    neg = true;
    q++;
  }
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
  return sd_str_new(out, n);
}
