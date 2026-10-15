/* Growing integers: the slow paths of the tagged sd_int.
 *
 * A big integer is sign + magnitude, the magnitude in 32-bit limbs (least
 * significant first). Every result is normalised: values that fit in the
 * small range are returned as small integers, so a bigint never holds a
 * small value. */
#include "seed_internal.h"

#include <math.h>

/* A read-only view of any integer as sign + magnitude. Small values use buf,
 * so a view must not be copied by value. */
typedef struct {
  int sign; /* -1, 0, 1 */
  int32_t n;
  const uint32_t *d;
  uint32_t buf[2];
} bview;

static void bv_init(bview *v, sd_int a) {
  if ((a.w & 1) == 0) {
    int64_t x = a.w >> 1;
    uint64_t m = x < 0 ? (uint64_t)0 - (uint64_t)x : (uint64_t)x;
    v->sign = x < 0 ? -1 : x > 0 ? 1 : 0;
    v->buf[0] = (uint32_t)m;
    v->buf[1] = (uint32_t)(m >> 32);
    v->n = m == 0 ? 0 : (m >> 32) ? 2 : 1;
    v->d = v->buf;
  } else {
    if (SD_UNLIKELY(a.w == 1)) sd_crash_null();
    sd_bigint *b = sd_int_big_(a);
    v->sign = b->sign;
    v->n = b->n;
    v->d = b->d;
  }
}

static sd_bigint *big_alloc(int64_t n) {
  if (n > INT32_MAX / 2) sd_crash("integer too large");
  sd_bigint *b = sd_obj_alloc(sizeof(sd_bigint) + (size_t)(n ? n : 1) * sizeof(uint32_t));
  b->rc = 1;
  b->sign = 1;
  b->n = (int32_t)n;
  return b;
}

void sd_bigint_free_(sd_bigint *b) { sd_obj_free(b); }

sd_bigint *sd_bigint_copy_(const sd_bigint *b) {
  sd_bigint *c = big_alloc(b->n);
  c->sign = b->sign;
  memcpy(c->d, b->d, (size_t)b->n * sizeof(uint32_t));
  return c;
}

/* Takes ownership of r (whose d holds n limbs) and returns the normalised
 * integer sign * |d|. */
static sd_int big_finish(sd_bigint *r, int sign, int32_t n) {
  while (n > 0 && r->d[n - 1] == 0) n--;
  if (n == 0) {
    sd_obj_free(r);
    return SD_INT(0);
  }
  if (n <= 2) {
    uint64_t m = r->d[0] | (n == 2 ? (uint64_t)r->d[1] << 32 : 0);
    if (sign > 0 && m <= (uint64_t)SD_INT_SMALL_MAX) {
      sd_obj_free(r);
      return SD_INT((int64_t)m);
    }
    if (sign < 0 && m <= (uint64_t)1 << 62) {
      sd_obj_free(r);
      return SD_INT((int64_t)(0 - m));
    }
  }
  r->sign = sign;
  r->n = n;
  return (sd_int){(int64_t)(uintptr_t)r | 1};
}

static sd_int from_view(const bview *v, int sign) {
  if (v->n == 0) return SD_INT(0);
  sd_bigint *r = big_alloc(v->n);
  memcpy(r->d, v->d, (size_t)v->n * sizeof(uint32_t));
  return big_finish(r, sign, v->n);
}

sd_int sd_int_from_u64(uint64_t m) {
  if (m <= (uint64_t)SD_INT_SMALL_MAX) return SD_INT((int64_t)m);
  sd_bigint *r = big_alloc(2);
  r->d[0] = (uint32_t)m;
  r->d[1] = (uint32_t)(m >> 32);
  return big_finish(r, 1, 2);
}

sd_int sd_int_from_i64(int64_t v) {
  if (v >= SD_INT_SMALL_MIN && v <= SD_INT_SMALL_MAX) return SD_INT(v);
  uint64_t m = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  sd_bigint *r = big_alloc(2);
  r->d[0] = (uint32_t)m;
  r->d[1] = (uint32_t)(m >> 32);
  return big_finish(r, v < 0 ? -1 : 1, 2);
}

bool sd_int_to_i64(sd_int a, int64_t *out) {
  if ((a.w & 1) == 0) {
    *out = a.w >> 1;
    return true;
  }
  if (a.w == 1) return false;
  sd_bigint *b = sd_int_big_(a);
  if (b->n > 2) return false;
  uint64_t m = b->d[0] | (b->n == 2 ? (uint64_t)b->d[1] << 32 : 0);
  if (b->sign > 0) {
    if (m > (uint64_t)INT64_MAX) return false;
    *out = (int64_t)m;
  } else {
    if (m > (uint64_t)INT64_MAX + 1) return false;
    *out = (int64_t)(0 - m);
  }
  return true;
}

/* ------------------------------------------------------- magnitude helpers */

static int mag_cmp(const uint32_t *a, int32_t an, const uint32_t *b, int32_t bn) {
  if (an != bn) return an < bn ? -1 : 1;
  for (int32_t i = an - 1; i >= 0; i--)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

/* r = a + b, an >= bn; r has room for an + 1 limbs. Returns an + 1. */
static int32_t mag_add(uint32_t *r, const uint32_t *a, int32_t an, const uint32_t *b, int32_t bn) {
  uint64_t c = 0;
  int32_t i = 0;
  for (; i < bn; i++) {
    c += (uint64_t)a[i] + b[i];
    r[i] = (uint32_t)c;
    c >>= 32;
  }
  for (; i < an; i++) {
    c += a[i];
    r[i] = (uint32_t)c;
    c >>= 32;
  }
  r[an] = (uint32_t)c;
  return an + 1;
}

/* r = a - b, a >= b. Returns an. */
static int32_t mag_sub(uint32_t *r, const uint32_t *a, int32_t an, const uint32_t *b, int32_t bn) {
  int64_t borrow = 0;
  int32_t i = 0;
  for (; i < bn; i++) {
    int64_t t = (int64_t)a[i] - b[i] - borrow;
    borrow = t < 0;
    r[i] = (uint32_t)t;
  }
  for (; i < an; i++) {
    int64_t t = (int64_t)a[i] - borrow;
    borrow = t < 0;
    r[i] = (uint32_t)t;
  }
  return an;
}

/* r (an + bn limbs, zeroed here) = a * b */
static void mag_mul(uint32_t *r, const uint32_t *a, int32_t an, const uint32_t *b, int32_t bn) {
  memset(r, 0, (size_t)(an + bn) * sizeof(uint32_t));
  for (int32_t i = 0; i < an; i++) {
    uint64_t c = 0, ai = a[i];
    if (ai == 0) continue;
    for (int32_t j = 0; j < bn; j++) {
      c += ai * b[j] + r[i + j];
      r[i + j] = (uint32_t)c;
      c >>= 32;
    }
    r[i + bn] = (uint32_t)c;
  }
}

/* x[0..xn) += y[0..yn), yn <= xn. Returns the carry out of the top. */
static uint32_t mag_add_to(uint32_t *x, int32_t xn, const uint32_t *y, int32_t yn) {
  uint64_t c = 0;
  int32_t i = 0;
  for (; i < yn; i++) {
    c += (uint64_t)x[i] + y[i];
    x[i] = (uint32_t)c;
    c >>= 32;
  }
  for (; c && i < xn; i++) {
    c += x[i];
    x[i] = (uint32_t)c;
    c >>= 32;
  }
  return (uint32_t)c;
}

/* x[0..xn) -= y[0..yn), yn <= xn. Returns the borrow out of the top. */
static uint32_t mag_sub_from(uint32_t *x, int32_t xn, const uint32_t *y, int32_t yn) {
  int64_t borrow = 0;
  int32_t i = 0;
  for (; i < yn; i++) {
    int64_t t = (int64_t)x[i] - y[i] - borrow;
    borrow = t < 0;
    x[i] = (uint32_t)t;
  }
  for (; borrow && i < xn; i++) {
    int64_t t = (int64_t)x[i] - borrow;
    borrow = t < 0;
    x[i] = (uint32_t)t;
  }
  return (uint32_t)borrow;
}

static int32_t mag_len(const uint32_t *a, int32_t n) {
  while (n > 0 && a[n - 1] == 0) n--;
  return n;
}

/* ------------------------------------------------ Karatsuba multiplication
 *
 * Long products are computed in words of 64 bits when the C compiler has a
 * 128-bit type (two limbs packed per word: a quarter of the multiplications
 * and half the additions of 32-bit limbs), else in the 32-bit limbs. The
 * operands are packed into words, multiplied with Karatsuba's method down to
 * the schoolbook product below KARA_MUL limbs (KARA_SQR for squares), and
 * the result is unpacked. The thresholds are in 32-bit limbs, measured with
 * the scratch program in runtime/tests/bench.c's style on x86_64. */
#ifndef KARA_MUL
#define KARA_MUL 64
#endif
#ifndef KARA_SQR
#define KARA_SQR 128
#endif

#if defined(__SIZEOF_INT128__) && !defined(SD_NO_INT128)
typedef uint64_t kw_t;
typedef unsigned __int128 kdw_t;
#define KW_BITS 64
#else
typedef uint32_t kw_t;
typedef uint64_t kdw_t;
#define KW_BITS 32
#endif
#define KW_LIMBS (KW_BITS / 32)
#define KW_MUL (KARA_MUL / KW_LIMBS)
#define KW_SQR (KARA_SQR / KW_LIMBS)

static int32_t kw_count(int32_t limbs) { return (limbs + KW_LIMBS - 1) / KW_LIMBS; }

static void kw_pack(kw_t *w, const uint32_t *a, int32_t n) {
#if KW_LIMBS == 2
  for (int32_t i = 0; 2 * i < n; i++) w[i] = a[2 * i] | (2 * i + 1 < n ? (kw_t)a[2 * i + 1] << 32 : 0);
#else
  memcpy(w, a, (size_t)n * sizeof(uint32_t));
#endif
}

/* r (n limbs) from the words w. */
static void kw_unpack(uint32_t *r, const kw_t *w, int32_t n) {
#if KW_LIMBS == 2
  for (int32_t i = 0; i < n; i++) r[i] = (uint32_t)(w[i / 2] >> (32 * (i & 1)));
#else
  memcpy(r, w, (size_t)n * sizeof(uint32_t));
#endif
}

static int32_t kw_len(const kw_t *a, int32_t n) {
  while (n > 0 && a[n - 1] == 0) n--;
  return n;
}

/* r (an + 1 words) = a + b, an >= bn. */
static void kw_add(kw_t *r, const kw_t *a, int32_t an, const kw_t *b, int32_t bn) {
  kdw_t c = 0;
  int32_t i = 0;
  for (; i < bn; i++) {
    c += (kdw_t)a[i] + b[i];
    r[i] = (kw_t)c;
    c >>= KW_BITS;
  }
  for (; i < an; i++) {
    c += a[i];
    r[i] = (kw_t)c;
    c >>= KW_BITS;
  }
  r[an] = (kw_t)c;
}

/* x[0..xn) += y[0..yn), yn <= xn; the sum fits. */
static void kw_add_to(kw_t *x, int32_t xn, const kw_t *y, int32_t yn) {
  kdw_t c = 0;
  int32_t i = 0;
  for (; i < yn; i++) {
    c += (kdw_t)x[i] + y[i];
    x[i] = (kw_t)c;
    c >>= KW_BITS;
  }
  for (; c && i < xn; i++) {
    c += x[i];
    x[i] = (kw_t)c;
    c >>= KW_BITS;
  }
}

/* x[0..xn) -= y[0..yn), yn <= xn; the difference is not negative. */
static void kw_sub_from(kw_t *x, int32_t xn, const kw_t *y, int32_t yn) {
  kw_t borrow = 0;
  int32_t i = 0;
  for (; i < yn; i++) {
    kdw_t d = (kdw_t)x[i] - y[i] - borrow;
    x[i] = (kw_t)d;
    borrow = (kw_t)(d >> KW_BITS) & 1;
  }
  for (; borrow && i < xn; i++) {
    borrow = x[i] == 0;
    x[i]--;
  }
}

/* The schoolbook product: r (an + bn words, zeroed here) = a * b. */
static void kw_mul_base(kw_t *r, const kw_t *a, int32_t an, const kw_t *b, int32_t bn) {
  memset(r, 0, (size_t)(an + bn) * sizeof(kw_t));
  for (int32_t i = 0; i < an; i++) {
    kdw_t c = 0;
    kw_t ai = a[i];
    if (ai == 0) continue;
    for (int32_t j = 0; j < bn; j++) {
      c += (kdw_t)ai * b[j] + r[i + j];
      r[i + j] = (kw_t)c;
      c >>= KW_BITS;
    }
    r[i + bn] = (kw_t)c;
  }
}

/* r (2n words, zeroed here) = a * a: each cross product once, doubled, plus
 * the squares on the diagonal. */
static void kw_sqr_base(kw_t *r, const kw_t *a, int32_t n) {
  memset(r, 0, (size_t)(2 * n) * sizeof(kw_t));
  for (int32_t i = 0; i < n; i++) {
    kdw_t c = 0;
    kw_t ai = a[i];
    if (ai == 0) continue;
    for (int32_t j = i + 1; j < n; j++) {
      c += (kdw_t)ai * a[j] + r[i + j];
      r[i + j] = (kw_t)c;
      c >>= KW_BITS;
    }
    r[i + n] = (kw_t)c;
  }
  kw_t top = 0;
  for (int32_t i = 0; i < 2 * n; i++) {
    kw_t v = r[i];
    r[i] = (v << 1) | top;
    top = v >> (KW_BITS - 1);
  }
  kdw_t c = 0;
  for (int32_t i = 0; i < n; i++) {
    kdw_t sq = (kdw_t)a[i] * a[i];
    c += (kdw_t)r[2 * i] + (kw_t)sq;
    r[2 * i] = (kw_t)c;
    c >>= KW_BITS;
    c += (kdw_t)r[2 * i + 1] + (kw_t)(sq >> KW_BITS);
    r[2 * i + 1] = (kw_t)c;
    c >>= KW_BITS;
  }
}

/* The scratch words that kara_mul and kara_sqr need for operands of up to n
 * words: each level uses 4h + 4 words (h = ceil(n / 2)) and recurses on
 * h + 1 words. */
static int64_t kara_scratch(int32_t n) {
  int64_t s = 0;
  while (n >= KW_MUL) {
    int32_t h = (n + 1) / 2;
    s += 4 * (int64_t)h + 4;
    n = h + 1;
  }
  return s;
}

/* r (an + bn words, an >= bn) = a * b, using scratch t (kara_scratch(an)
 * words). r must not overlap a, b or t. The operands may have leading zero
 * words. */
static void kara_mul(kw_t *r, const kw_t *a, int32_t an, const kw_t *b, int32_t bn, kw_t *t) {
  if (bn < KW_MUL) {
    kw_mul_base(r, a, an, b, bn);
    return;
  }
  int32_t h = (an + 1) / 2;
  if (bn <= h) {
    /* Unbalanced: a in slices of bn words, each times b. */
    kw_t *p = t, *tt = t + 2 * (int64_t)bn;
    memset(r, 0, (size_t)(an + bn) * sizeof(kw_t));
    for (int32_t i = 0; i < an; i += bn) {
      int32_t cn = an - i < bn ? an - i : bn;
      if (cn == bn) kara_mul(p, a + i, cn, b, bn, tt);
      else kara_mul(p, b, bn, a + i, cn, tt);
      kw_add_to(r + i, an + bn - i, p, cn + bn);
    }
    return;
  }
  /* a = a1 W^h + a0, b = b1 W^h + b0;
   * a b = a1 b1 W^2h + ((a0 + a1)(b0 + b1) - a0 b0 - a1 b1) W^h + a0 b0 */
  int32_t a1n = an - h, b1n = bn - h;
  kara_mul(r, a, h, b, h, t);
  kara_mul(r + 2 * h, a + h, a1n, b + h, b1n, t);
  kw_t *sa = t, *sb = t + h + 1, *z = t + 2 * (h + 1), *tt = z + 2 * (h + 1);
  kw_add(sa, a, h, a + h, a1n);
  kw_add(sb, b, h, b + h, b1n);
  kara_mul(z, sa, h + 1, sb, h + 1, tt);
  kw_sub_from(z, 2 * h + 2, r, 2 * h);
  kw_sub_from(z, 2 * h + 2, r + 2 * h, an + bn - 2 * h);
  kw_add_to(r + h, an + bn - h, z, kw_len(z, 2 * h + 2));
}

/* r (2n words) = a * a, with scratch t (kara_scratch(n) words). */
static void kara_sqr(kw_t *r, const kw_t *a, int32_t n, kw_t *t) {
  if (n < KW_SQR) {
    kw_sqr_base(r, a, n);
    return;
  }
  int32_t h = (n + 1) / 2, a1n = n - h;
  kara_sqr(r, a, h, t);
  kara_sqr(r + 2 * h, a + h, a1n, t);
  kw_t *sa = t, *z = t + h + 1, *tt = z + 2 * (h + 1);
  kw_add(sa, a, h, a + h, a1n);
  kara_sqr(z, sa, h + 1, tt);
  kw_sub_from(z, 2 * h + 2, r, 2 * h);
  kw_sub_from(z, 2 * h + 2, r + 2 * h, 2 * n - 2 * h);
  kw_add_to(r + h, 2 * n - h, z, kw_len(z, 2 * h + 2));
}

/* r (an + 1 limbs) = a * m (two limbs at a time with 64-bit words). */
static void mag_mul_1(uint32_t *r, const uint32_t *a, int32_t an, uint32_t m) {
  uint64_t c = 0;
  int32_t i = 0;
#if KW_LIMBS == 2
  for (; i + 1 < an; i += 2) {
    kdw_t t = (kdw_t)(a[i] | (uint64_t)a[i + 1] << 32) * m + c;
    r[i] = (uint32_t)t;
    r[i + 1] = (uint32_t)((uint64_t)t >> 32);
    c = (uint64_t)(t >> 64);
  }
#endif
  for (; i < an; i++) {
    c += (uint64_t)a[i] * m;
    r[i] = (uint32_t)c;
    c >>= 32;
  }
  r[an] = (uint32_t)c;
}

/* r (an + bn limbs) = a * b, by the fastest method for the sizes. r must
 * not overlap a or b. */
static void mag_mul_fast(uint32_t *r, const uint32_t *a, int32_t an, const uint32_t *b, int32_t bn) {
  if (an < bn) {
    const uint32_t *x = a;
    a = b;
    b = x;
    int32_t xn = an;
    an = bn;
    bn = xn;
  }
  if (bn == 1) {
    mag_mul_1(r, a, an, b[0]);
    return;
  }
  if (bn < 4) {
    mag_mul(r, a, an, b, bn);
    return;
  }
  bool square = a == b && an == bn;
  int32_t wa = kw_count(an), wb = square ? 0 : kw_count(bn), wr = kw_count(an) + kw_count(bn);
  int64_t total = wa + wb + wr + kara_scratch(wa);
  kw_t local[192];
  kw_t *w = total <= 192 ? local : sd_alloc((size_t)total * sizeof(kw_t));
  kw_t *pa = w, *pb = w + wa, *pr = pb + wb, *t = pr + wr;
  kw_pack(pa, a, an);
  if (square) {
    kara_sqr(pr, pa, wa, t);
  } else {
    kw_pack(pb, b, bn);
    kara_mul(pr, pa, wa, pb, kw_count(bn), t);
  }
  kw_unpack(r, pr, an + bn);
  if (w != local) free(w);
}

/* q = a / d (q may alias a); returns the remainder. */
static uint32_t mag_divsmall(uint32_t *q, const uint32_t *a, int32_t n, uint32_t d) {
  uint64_t rem = 0;
  for (int32_t i = n - 1; i >= 0; i--) {
    uint64_t cur = (rem << 32) | a[i];
    q[i] = (uint32_t)(cur / d);
    rem = cur % d;
  }
  return (uint32_t)rem;
}

/* d[0..*n) = d * mul + add; d has room for one more limb. */
static void mag_mul_small_add(uint32_t *d, int32_t *n, uint32_t mul, uint32_t add) {
  uint64_t c = add;
  for (int32_t i = 0; i < *n; i++) {
    c += (uint64_t)d[i] * mul;
    d[i] = (uint32_t)c;
    c >>= 32;
  }
  if (c) d[(*n)++] = (uint32_t)c;
}

/* Knuth's algorithm D (after Hacker's Delight, divmnu). u has m limbs, v has
 * n >= 2 limbs with v[n-1] != 0, m >= n. q gets m - n + 1 limbs, r (if not
 * NULL) gets n limbs. */
static void knuth_core(uint32_t *q, uint32_t *un, int32_t qn, const uint32_t *vn, int32_t n);

static void mag_divmod(uint32_t *q, uint32_t *r, const uint32_t *u, int32_t m, const uint32_t *v, int32_t n) {
  uint32_t *un = sd_alloc((size_t)(m + 1) * sizeof(uint32_t));
  uint32_t *vn = sd_alloc((size_t)n * sizeof(uint32_t));
  int s = __builtin_clz(v[n - 1]);
  for (int32_t i = n - 1; i > 0; i--) vn[i] = (v[i] << s) | (uint32_t)((uint64_t)v[i - 1] >> (32 - s));
  vn[0] = v[0] << s;
  un[m] = (uint32_t)((uint64_t)u[m - 1] >> (32 - s));
  for (int32_t i = m - 1; i > 0; i--) un[i] = (u[i] << s) | (uint32_t)((uint64_t)u[i - 1] >> (32 - s));
  un[0] = u[0] << s;
  knuth_core(q, un, m - n + 1, vn, n);
  if (r)
    for (int32_t i = 0; i < n; i++) r[i] = (un[i] >> s) | (uint32_t)((uint64_t)un[i + 1] << (32 - s));
  free(un);
  free(vn);
}

/* The loop of algorithm D on normalised operands: un has qn + n limbs and
 * its top n limbs are less than vn, which has n >= 2 limbs and its top bit
 * set. q (qn limbs) gets un / vn, and un[0..n) the remainder. */
static void knuth_core(uint32_t *q, uint32_t *un, int32_t qn, const uint32_t *vn, int32_t n) {
  const uint64_t b = (uint64_t)1 << 32;
  for (int32_t j = qn - 1; j >= 0; j--) {
    uint64_t num = ((uint64_t)un[j + n] << 32) | un[j + n - 1];
    uint64_t qhat = num / vn[n - 1];
    uint64_t rhat = num - qhat * vn[n - 1];
    while (qhat >= b || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
      qhat--;
      rhat += vn[n - 1];
      if (rhat >= b) break;
    }
    int64_t k = 0, t;
    for (int32_t i = 0; i < n; i++) {
      uint64_t p = qhat * vn[i];
      t = (int64_t)un[i + j] - k - (int64_t)(p & 0xFFFFFFFFu);
      un[i + j] = (uint32_t)t;
      k = (int64_t)(p >> 32) - (t >> 32);
    }
    t = (int64_t)un[j + n] - k;
    un[j + n] = (uint32_t)t;
    q[j] = (uint32_t)qhat;
    if (t < 0) {
      q[j]--;
      uint64_t c = 0;
      for (int32_t i = 0; i < n; i++) {
        c += (uint64_t)un[i + j] + vn[i];
        un[i + j] = (uint32_t)c;
        c >>= 32;
      }
      un[j + n] += (uint32_t)c;
    }
  }
}

/* Burnikel and Ziegler's recursive division ("Fast recursive division",
 * 1998), which divides in about twice the time of a multiplication of the
 * same size (so with Karatsuba, in O(n^1.6 log n) instead of O(n^2)).
 * Divisors shorter than BZ_DIV limbs, or quotients shorter than BZ_DIV
 * limbs, use algorithm D; so do the recursion's leaves under BZ_LEAF
 * limbs. */
#ifndef BZ_DIV
#define BZ_DIV 80
#endif
#ifndef BZ_LEAF
#define BZ_LEAF 40
#endif

static void bz_3by2(uint32_t *q, uint32_t *r, const uint32_t *a, const uint32_t *b, int32_t n);
static int32_t mag_shr(uint32_t *r, const uint32_t *d, int32_t n, int64_t k);

/* q (n limbs) = a / b and r (n limbs) = a % b, where a has 2n limbs and is
 * less than b β^n, and b has n limbs and its top bit set. */
static void bz_2by1(uint32_t *q, uint32_t *r, const uint32_t *a, const uint32_t *b, int32_t n) {
  if ((n & 1) || n < BZ_LEAF) {
    uint32_t *u = sd_alloc((size_t)(2 * n) * sizeof(uint32_t));
    memcpy(u, a, (size_t)(2 * n) * sizeof(uint32_t));
    knuth_core(q, u, n, b, n);
    memcpy(r, u, (size_t)n * sizeof(uint32_t));
    free(u);
    return;
  }
  /* a = [a1 a2 a3 a4] in halves of b's length, most significant first:
   * [a1 a2 a3] / b gives the high half of q and a remainder r1, then
   * [r1 a4] / b the low half and r. */
  int32_t h = n / 2;
  uint32_t *t = sd_alloc((size_t)(3 * h) * sizeof(uint32_t));
  bz_3by2(q + h, t + h, a + h, b, h);
  memcpy(t, a, (size_t)h * sizeof(uint32_t));
  bz_3by2(q, r, t, b, h);
  free(t);
}

/* q (n limbs) = a / b and r (2n limbs) = a % b, where a has 3n limbs and is
 * less than b β^n, and b has 2n limbs and its top bit set. */
static void bz_3by2(uint32_t *q, uint32_t *r, const uint32_t *a, const uint32_t *b, int32_t n) {
  const uint32_t *b1 = b + n, *b2 = b, *a1 = a + 2 * n;
  /* rr = [r1 a3] in 2n + 1 limbs, where r1 = [a1 a2] - q b1 for the
   * estimate q = min([a1 a2] / b1, β^n - 1), which is at most 2 too large. */
  uint32_t *rr = sd_alloc((size_t)(2 * n + 1) * sizeof(uint32_t));
  memcpy(rr, a, (size_t)n * sizeof(uint32_t));
  if (mag_cmp(a1, n, b1, n) < 0) {
    bz_2by1(q, rr + n, a + n, b1, n);
    rr[2 * n] = 0;
  } else {
    /* a1 == b1 here (a < b β^n), so r1 = [a1 a2] - (β^n - 1) b1 = a2 + b1. */
    memset(q, 0xFF, (size_t)n * sizeof(uint32_t));
    mag_add(rr + n, a + n, n, b1, n);
  }
  uint32_t *d = sd_alloc((size_t)(2 * n) * sizeof(uint32_t));
  mag_mul_fast(d, q, n, b2, n);
  /* rr -= q b2; while that is negative (a borrow out of the top limb that
   * no carry has cancelled yet), add b back and decrement q. */
  bool negative = mag_sub_from(rr, 2 * n + 1, d, 2 * n) != 0;
  while (negative) {
    if (mag_add_to(rr, 2 * n + 1, b, 2 * n)) negative = false;
    uint32_t one = 1;
    mag_sub_from(q, n, &one, 1);
  }
  memcpy(r, rr, (size_t)(2 * n) * sizeof(uint32_t));
  free(d);
  free(rr);
}

/* Like mag_divmod (u has m limbs, v has n >= 2 limbs with v[n-1] != 0,
 * m >= n; q gets m - n + 1 limbs, r if not NULL n limbs), by recursive
 * division: v is shifted left to a block size of nn limbs (a power of two
 * times a leaf size) with its top bit set, u by the same amount and cut
 * into blocks of nn limbs, and each block is divided with bz_2by1. */
static void bz_divmod(uint32_t *q, uint32_t *r, const uint32_t *u, int32_t m, const uint32_t *v, int32_t n) {
  int32_t pow2 = 1;
  while (n / pow2 >= BZ_LEAF) pow2 *= 2;
  int32_t nn = (n + pow2 - 1) / pow2 * pow2;
  int64_t shift = (int64_t)(nn - n) * 32 + __builtin_clz(v[n - 1]);
  int32_t limbs = (int32_t)(shift / 32), bits = (int)(shift % 32);
  /* vb = v << shift (exactly nn limbs) */
  uint32_t *vb = sd_alloc((size_t)nn * sizeof(uint32_t));
  memset(vb, 0, (size_t)limbs * sizeof(uint32_t));
  for (int32_t i = 0; i < n; i++)
    vb[limbs + i] = (v[i] << bits) | (bits && i > 0 ? v[i - 1] >> (32 - bits) : 0);
  /* ub = u << shift, in t blocks: enough that the top bit of the top block
   * is clear, so that the top block is less than vb. */
  int64_t ubits = (int64_t)(m - 1) * 32 + (32 - __builtin_clz(u[m - 1])) + shift;
  int64_t blockbits = (int64_t)nn * 32;
  int32_t t = (int32_t)((ubits + 1 + blockbits - 1) / blockbits);
  if (t < 2) t = 2;
  int64_t ulen = (int64_t)t * nn;
  uint32_t *ub = sd_alloc((size_t)ulen * sizeof(uint32_t));
  memset(ub, 0, (size_t)ulen * sizeof(uint32_t));
  for (int32_t i = 0; i < m; i++) {
    ub[limbs + i] |= u[i] << bits;
    if (bits && limbs + i + 1 < ulen) ub[limbs + i + 1] |= u[i] >> (32 - bits);
  }
  int64_t qlen = (int64_t)(t - 1) * nn;
  uint32_t *qb = sd_alloc((size_t)qlen * sizeof(uint32_t));
  uint32_t *z = sd_alloc((size_t)(2 * nn) * sizeof(uint32_t));
  uint32_t *rem = sd_alloc((size_t)nn * sizeof(uint32_t));
  memcpy(z, ub + (int64_t)(t - 2) * nn, (size_t)(2 * nn) * sizeof(uint32_t));
  for (int32_t i = t - 2; i >= 0; i--) {
    bz_2by1(qb + (int64_t)i * nn, rem, z, vb, nn);
    if (i > 0) {
      memcpy(z + nn, rem, (size_t)nn * sizeof(uint32_t));
      memcpy(z, ub + (int64_t)(i - 1) * nn, (size_t)nn * sizeof(uint32_t));
    }
  }
  int64_t qn = (int64_t)m - n + 1;
  for (int64_t i = 0; i < qn; i++) q[i] = i < qlen ? qb[i] : 0;
  if (r) mag_shr(r, rem, nn, shift);
  free(rem);
  free(z);
  free(qb);
  free(ub);
  free(vb);
}

/* mag_divmod by the fastest method for the sizes. */
static void mag_divmod_fast(uint32_t *q, uint32_t *r, const uint32_t *u, int32_t m, const uint32_t *v, int32_t n) {
  if (n >= BZ_DIV && m - n >= BZ_DIV) bz_divmod(q, r, u, m, v, n);
  else mag_divmod(q, r, u, m, v, n);
}

/* ----------------------------------------------------------- arithmetic */

static sd_int add_views(const bview *a, const bview *b, int bsign) {
  if (b->n == 0) return from_view(a, a->sign);
  if (a->n == 0) return from_view(b, bsign);
  if (a->sign == bsign) {
    const bview *x = a->n >= b->n ? a : b, *y = a->n >= b->n ? b : a;
    sd_bigint *r = big_alloc(x->n + 1);
    int32_t n = mag_add(r->d, x->d, x->n, y->d, y->n);
    return big_finish(r, bsign, n);
  }
  int c = mag_cmp(a->d, a->n, b->d, b->n);
  if (c == 0) return SD_INT(0);
  const bview *x = c > 0 ? a : b, *y = c > 0 ? b : a;
  sd_bigint *r = big_alloc(x->n);
  int32_t n = mag_sub(r->d, x->d, x->n, y->d, y->n);
  return big_finish(r, c > 0 ? a->sign : bsign, n);
}

sd_int sd_int_add_slow(sd_int a, sd_int b) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  return add_views(&x, &y, y.sign);
}

sd_int sd_int_sub_slow(sd_int a, sd_int b) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  return add_views(&x, &y, -y.sign);
}

sd_int sd_int_neg_slow(sd_int a) {
  bview x;
  bv_init(&x, a);
  return from_view(&x, -x.sign);
}

static sd_int int_mul(sd_int a, sd_int b, bool fast) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  if (x.n == 0 || y.n == 0) return SD_INT(0);
  sd_bigint *r = big_alloc((int64_t)x.n + y.n);
  if (fast) mag_mul_fast(r->d, x.d, x.n, y.d, y.n);
  else mag_mul(r->d, x.d, x.n, y.d, y.n);
  return big_finish(r, x.sign * y.sign, x.n + y.n);
}

sd_int sd_int_mul_slow(sd_int a, sd_int b) { return int_mul(a, b, true); }

void sd_crash_div_zero(void) { sd_crash("division by zero"); }

/* Truncating division: q = trunc(a / b), r = a - q * b. With fast false,
 * only the schoolbook algorithms (for the tests). */
static void int_divmod_by(sd_int a, sd_int b, sd_int *q, sd_int *r, bool fast) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  if (y.n == 0) sd_crash_div_zero();
  if (mag_cmp(x.d, x.n, y.d, y.n) < 0) {
    if (q) *q = SD_INT(0);
    if (r) *r = from_view(&x, x.sign);
    return;
  }
  sd_bigint *qq = big_alloc(x.n);
  if (y.n == 1) {
    uint32_t rem = mag_divsmall(qq->d, x.d, x.n, y.d[0]);
    if (q) *q = big_finish(qq, x.sign * y.sign, x.n);
    else sd_obj_free(qq);
    if (r) *r = sd_int_from_i64(x.sign < 0 ? -(int64_t)rem : (int64_t)rem);
    return;
  }
  sd_bigint *rr = r ? big_alloc(y.n) : NULL;
  if (fast) mag_divmod_fast(qq->d, rr ? rr->d : NULL, x.d, x.n, y.d, y.n);
  else mag_divmod(qq->d, rr ? rr->d : NULL, x.d, x.n, y.d, y.n);
  if (q) *q = big_finish(qq, x.sign * y.sign, x.n - y.n + 1);
  else sd_obj_free(qq);
  if (r) *r = big_finish(rr, x.sign, y.n);
}

static void int_divmod(sd_int a, sd_int b, sd_int *q, sd_int *r) { int_divmod_by(a, b, q, r, true); }

sd_int sd_int_mul_basecase_(sd_int a, sd_int b) { return int_mul(a, b, false); }

void sd_int_divmod_basecase_(sd_int a, sd_int b, sd_int *q, sd_int *r) { int_divmod_by(a, b, q, r, false); }

sd_int sd_int_div_slow(sd_int a, sd_int b) {
  sd_int q;
  int_divmod(a, b, &q, NULL);
  return q;
}

sd_int sd_int_mod_slow(sd_int a, sd_int b) {
  sd_int r;
  int_divmod(a, b, NULL, &r);
  return r;
}

bool sd_int_eq_slow(sd_int a, sd_int b) {
  if ((a.w & 1) == 0 || (b.w & 1) == 0 || a.w == 1 || b.w == 1) return a.w == b.w;
  sd_bigint *x = sd_int_big_(a), *y = sd_int_big_(b);
  return x->sign == y->sign && x->n == y->n && memcmp(x->d, y->d, (size_t)x->n * sizeof(uint32_t)) == 0;
}

int sd_int_cmp_slow(sd_int a, sd_int b) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  if (x.sign != y.sign) return x.sign < y.sign ? -1 : 1;
  int c = mag_cmp(x.d, x.n, y.d, y.n);
  return x.sign < 0 ? -c : c;
}

/* ------------------------------------------------------------- bitwise */

/* out (L limbs) = x in two's complement. */
static void to_twos(uint32_t *out, int32_t L, const bview *x) {
  for (int32_t i = 0; i < L; i++) out[i] = i < x->n ? x->d[i] : 0;
  if (x->sign < 0) {
    uint64_t c = 1;
    for (int32_t i = 0; i < L; i++) {
      c += (uint32_t)~out[i];
      out[i] = (uint32_t)c;
      c >>= 32;
    }
  }
}

static sd_int bitop(sd_int a, sd_int b, int op) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  int32_t L = (x.n > y.n ? x.n : y.n) + 1;
  sd_bigint *r = big_alloc(L);
  uint32_t *t = sd_alloc((size_t)L * sizeof(uint32_t));
  to_twos(r->d, L, &x);
  to_twos(t, L, &y);
  for (int32_t i = 0; i < L; i++) {
    switch (op) {
    case 0: r->d[i] &= t[i]; break;
    case 1: r->d[i] |= t[i]; break;
    default: r->d[i] ^= t[i]; break;
    }
  }
  free(t);
  bool neg = (r->d[L - 1] >> 31) != 0;
  if (neg) {
    uint64_t c = 1;
    for (int32_t i = 0; i < L; i++) {
      c += (uint32_t)~r->d[i];
      r->d[i] = (uint32_t)c;
      c >>= 32;
    }
  }
  return big_finish(r, neg ? -1 : 1, L);
}

sd_int sd_int_and_slow(sd_int a, sd_int b) { return bitop(a, b, 0); }
sd_int sd_int_or_slow(sd_int a, sd_int b) { return bitop(a, b, 1); }
sd_int sd_int_xor_slow(sd_int a, sd_int b) { return bitop(a, b, 2); }

/* The shift count as an int64, crashing when negative. Returns false when it
 * is too large for an int64. */
static bool shift_count(sd_int b, int64_t *k) {
  if (sd_int_cmp(b, SD_INT(0)) < 0) sd_crash("negative shift count");
  return sd_int_to_i64(b, k);
}

static sd_int mag_shl(const bview *x, int sign, int64_t k) {
  int64_t limbs = k / 32;
  int bits = (int)(k % 32);
  sd_bigint *r = big_alloc(x->n + limbs + 1);
  memset(r->d, 0, (size_t)limbs * sizeof(uint32_t));
  uint32_t carry = 0;
  for (int32_t i = 0; i < x->n; i++) {
    r->d[limbs + i] = (x->d[i] << bits) | carry;
    carry = bits ? x->d[i] >> (32 - bits) : 0;
  }
  r->d[limbs + x->n] = carry;
  return big_finish(r, sign, (int32_t)(x->n + limbs + 1));
}

/* r = x >> k (magnitude); r has x->n limbs. Returns the limb count. */
static int32_t mag_shr(uint32_t *r, const uint32_t *d, int32_t n, int64_t k) {
  int64_t limbs = k / 32;
  int bits = (int)(k % 32);
  if (limbs >= n) return 0;
  int32_t rn = (int32_t)(n - limbs);
  for (int32_t i = 0; i < rn; i++) {
    uint32_t lo = d[i + limbs] >> bits;
    uint32_t hi = (bits && i + limbs + 1 < n) ? d[i + limbs + 1] << (32 - bits) : 0;
    r[i] = lo | hi;
  }
  return rn;
}

sd_int sd_int_shl_slow(sd_int a, sd_int b) {
  int64_t k;
  bool fits = shift_count(b, &k);
  bview x;
  bv_init(&x, a);
  if (x.n == 0) return SD_INT(0);
  if (!fits || k > ((int64_t)1 << 32)) sd_crash("shift count too large");
  return mag_shl(&x, x.sign, k);
}

sd_int sd_int_shr_slow(sd_int a, sd_int b) {
  int64_t k;
  bool fits = shift_count(b, &k);
  bview x;
  bv_init(&x, a);
  if (x.n == 0) return SD_INT(0);
  if (!fits || k >= (int64_t)x.n * 32) return SD_INT(x.sign < 0 ? -1 : 0);
  if (x.sign > 0) {
    sd_bigint *r = big_alloc(x.n);
    int32_t n = mag_shr(r->d, x.d, x.n, k);
    return big_finish(r, 1, n);
  }
  /* floor for negatives: -(((|x| - 1) >> k) + 1) */
  uint32_t *t = sd_alloc((size_t)x.n * sizeof(uint32_t));
  uint32_t one = 1;
  mag_sub(t, x.d, x.n, &one, 1);
  sd_bigint *r = big_alloc(x.n + 1);
  int32_t n = mag_shr(r->d, t, x.n, k);
  free(t);
  while (n > 0 && r->d[n - 1] == 0) n--;
  /* + 1 */
  uint64_t c = 1;
  for (int32_t i = 0; i < n && c; i++) {
    c += r->d[i];
    r->d[i] = (uint32_t)c;
    c >>= 32;
  }
  if (c) r->d[n++] = (uint32_t)c;
  if (n == 0) {
    r->d[0] = 1;
    n = 1;
  }
  return big_finish(r, -1, n);
}

/* --------------------------------------------------------- conversions */

double sd_int_to_float(sd_int a) {
  if ((a.w & 1) == 0) return (double)(a.w >> 1);
  bview x;
  bv_init(&x, a);
  int32_t n = x.n;
  int64_t bits = (int64_t)(n - 1) * 32 + (32 - __builtin_clz(x.d[n - 1]));
  if (bits <= 64) {
    uint64_t m = x.d[0] | (n > 1 ? (uint64_t)x.d[1] << 32 : 0);
    return x.sign * (double)m;
  }
  /* The top 64 bits, with a sticky bit for everything below, round
   * correctly when converted to double. */
  int64_t shift = bits - 64;
  int32_t li = (int32_t)(shift / 32);
  int bi = (int)(shift % 32);
  uint64_t lo = x.d[li], mid = li + 1 < n ? x.d[li + 1] : 0, hi = li + 2 < n ? x.d[li + 2] : 0;
  uint64_t m = (lo >> bi) | (mid << (32 - bi)) | (bi ? hi << (64 - bi) : 0);
  bool sticky = (bi && (lo & (((uint64_t)1 << bi) - 1))) != 0;
  for (int32_t i = 0; i < li && !sticky; i++) sticky = x.d[i] != 0;
  if (sticky) m |= 1;
  return x.sign * ldexp((double)m, (int)(shift > 100000 ? 100000 : shift));
}

sd_int sd_float_to_int(double f) {
  if (isnan(f) || isinf(f)) {
    sd_crash("cannot convert float %s to int", isnan(f) ? "nan" : f > 0 ? "inf" : "-inf");
  }
  if (fabs(f) < 4611686018427387904.0) return SD_INT((int64_t)f); /* 2^62 */
  int e;
  double m = frexp(fabs(f), &e); /* |f| = m * 2^e, 0.5 <= m < 1 */
  uint64_t mant = (uint64_t)ldexp(m, 53);
  bview x;
  bv_init(&x, SD_INT(0));
  x.buf[0] = (uint32_t)mant;
  x.buf[1] = (uint32_t)(mant >> 32);
  x.n = x.buf[1] ? 2 : 1;
  x.sign = 1;
  return mag_shl(&x, f < 0 ? -1 : 1, e - 53);
}

uint8_t sd_int_to_byte(sd_int a) {
  if ((a.w & 1) == 0 && a.w >= 0 && a.w <= 2 * 255) return (uint8_t)(a.w >> 1);
  char *s = sd_int_to_cstr(a);
  char *m = sd_fmt("cannot convert %s to byte", s);
  free(s);
  sd_crash_owned(m);
}

/* ----------------------------------------------------------- text */

/* Text conversion of long numbers divides and conquers with the powers of
 * ten 10^(9 * 2^k) (pow10[k], 2^k limbs of base 10^9): a number is split by
 * one of about half its length, and the halves are converted recursively,
 * which costs a few divisions or multiplications of each size, so
 * O(M(n) log n) instead of the schoolbook O(n^2). Below these sizes the
 * schoolbook loops are faster. */
#ifndef TEXT_OUT_LEAF
#define TEXT_OUT_LEAF 30 /* limbs */
#endif
#ifndef TEXT_IN_LEAF
#define TEXT_IN_LEAF 600 /* digits */
#endif

typedef struct {
  uint32_t *d;
  int32_t n;
} pow10_t;

/* Adds the next power of ten to the table (pw[k] = pw[k-1]^2). */
static void pow10_next(pow10_t *pw, int k) {
  if (k == 0) {
    pw[0].d = sd_alloc(sizeof(uint32_t));
    pw[0].d[0] = 1000000000u;
    pw[0].n = 1;
    return;
  }
  int32_t n = pw[k - 1].n;
  uint32_t *d = sd_alloc((size_t)(2 * n) * sizeof(uint32_t));
  mag_mul_fast(d, pw[k - 1].d, n, pw[k - 1].d, n);
  pw[k].d = d;
  pw[k].n = mag_len(d, 2 * n);
}

static void pow10_free(pow10_t *pw, int count) {
  for (int k = 0; k < count; k++) free(pw[k].d);
}

/* Writes x (n limbs, x < 10^width) as exactly width digits, zero-padded on
 * the left, using limbs of base 10^9 by repeated short division. */
static void to_dec_basecase(const uint32_t *x, int32_t n, char *out, int64_t width) {
  uint32_t *t = sd_alloc((size_t)(n ? n : 1) * sizeof(uint32_t));
  memcpy(t, x, (size_t)n * sizeof(uint32_t));
  n = mag_len(t, n);
  int64_t pos = width;
  while (n > 0) {
    uint32_t c = mag_divsmall(t, t, n, 1000000000u);
    n = mag_len(t, n);
    for (int i = 0; i < 9 && pos > 0; i++) {
      out[--pos] = (char)('0' + c % 10);
      c /= 10;
    }
  }
  memset(out, '0', (size_t)pos);
  free(t);
}

/* The same by divide and conquer; pw holds the powers of ten up to pw[k]. */
static void to_dec(const uint32_t *x, int32_t n, char *out, int64_t width, const pow10_t *pw, int k) {
  n = mag_len(x, n);
  if (n < TEXT_OUT_LEAF) {
    to_dec_basecase(x, n, out, width);
    return;
  }
  /* The largest power with about half the limbs of x: since it is less than
   * x, x has more digits than the power's 9 * 2^k zeros. */
  while (k > 0 && 2 * pw[k].n > n + 1) k--;
  const pow10_t *p = &pw[k];
  int32_t qn = n - p->n + 1;
  uint32_t *q = sd_alloc((size_t)qn * sizeof(uint32_t));
  uint32_t *r = sd_alloc((size_t)p->n * sizeof(uint32_t));
  if (p->n == 1) r[0] = mag_divsmall(q, x, n, p->d[0]);
  else mag_divmod_fast(q, r, x, n, p->d, p->n);
  int64_t low = (int64_t)9 << k;
  to_dec(q, qn, out, width - low, pw, k);
  free(q);
  to_dec(r, p->n, out + width - low, low, pw, k - 1 < 0 ? 0 : k - 1);
  free(r);
}

static char *to_cstr(sd_int a, bool fast) {
  if ((a.w & 1) == 0) return sd_fmt("%lld", (long long)(a.w >> 1));
  if (a.w == 1) return sd_fmt("null");
  bview x;
  bv_init(&x, a);
  int32_t n = x.n;
  /* x < 2^bits <= 10^width, as log10(2) < 0.30103 */
  int64_t bits = (int64_t)(n - 1) * 32 + (32 - __builtin_clz(x.d[n - 1]));
  int64_t width = bits * 30103 / 100000 + 1;
  char *s = sd_alloc((size_t)width + 2);
  char *digits = s + 1;
  if (!fast || n < TEXT_OUT_LEAF) {
    to_dec_basecase(x.d, n, digits, width);
  } else {
    pow10_t pw[40];
    int count = 0;
    pow10_next(pw, count++);
    /* to_dec uses powers of at most (n + 1) / 2 limbs; a square of m limbs
     * has at least 2m - 1. */
    while (2 * (2 * pw[count - 1].n - 1) <= n + 1) pow10_next(pw, count++);
    to_dec(x.d, n, digits, width, pw, count - 1);
    pow10_free(pw, count);
  }
  int64_t skip = 0;
  while (skip < width - 1 && digits[skip] == '0') skip++;
  char *p = s;
  if (x.sign < 0) *p++ = '-';
  memmove(p, digits + skip, (size_t)(width - skip));
  p[width - skip] = 0;
  return s;
}

char *sd_int_to_cstr(sd_int a) { return to_cstr(a, true); }

char *sd_int_to_cstr_basecase_(sd_int a) { return to_cstr(a, false); }

/* The value of the decimal digits p[0..len) into r (room for len / 9 + 2
 * limbs). Returns the limb count. */
static int32_t from_dec_basecase(const char *p, int64_t len, uint32_t *r) {
  int32_t n = 0;
  uint32_t chunk = 0, mul = 1;
  int inchunk = 0;
  for (int64_t i = 0; i < len; i++) {
    chunk = chunk * 10 + (uint32_t)(p[i] - '0');
    mul *= 10;
    if (++inchunk == 9) {
      mag_mul_small_add(r, &n, mul, chunk);
      chunk = 0;
      mul = 1;
      inchunk = 0;
    }
  }
  if (inchunk) mag_mul_small_add(r, &n, mul, chunk);
  return n;
}

/* The same by divide and conquer, with the powers of ten up to pw[k] (each
 * at least 9 * 2^k < len digits are split off at the bottom). r has room for
 * len / 9 + 2 limbs. */
static int32_t from_dec(const char *p, int64_t len, uint32_t *r, const pow10_t *pw, int k) {
  if (len <= TEXT_IN_LEAF) return from_dec_basecase(p, len, r);
  while (k > 0 && ((int64_t)9 << k) >= len) k--;
  int64_t low = (int64_t)9 << k, high = len - low;
  uint32_t *h = sd_alloc((size_t)(high / 9 + 2) * sizeof(uint32_t));
  int32_t hn = from_dec(p, high, h, pw, k);
  int32_t ln = from_dec(p + high, low, r, pw, k);
  /* r = h * 10^low + r */
  int32_t pn = pw[k].n, rn = hn + pn;
  uint32_t *prod = sd_alloc((size_t)(rn + 1) * sizeof(uint32_t));
  if (hn > 0) {
    mag_mul_fast(prod, h, hn, pw[k].d, pn);
  } else {
    memset(prod, 0, (size_t)rn * sizeof(uint32_t));
  }
  prod[rn] = 0;
  mag_add_to(prod, rn + 1, r, ln);
  rn = mag_len(prod, rn + 1);
  memcpy(r, prod, (size_t)rn * sizeof(uint32_t));
  free(prod);
  free(h);
  return rn;
}

/* Parses [-]digits (base 10) or [-]0x hexdigits (when allow_hex). With fast
 * false, only the schoolbook algorithm (for the tests). */
static bool parse_int_by(const char *p, int64_t len, bool allow_hex, sd_int *out, bool fast) {
  int sign = 1;
  int64_t i = 0;
  if (len > 0 && p[0] == '-') {
    sign = -1;
    i = 1;
  }
  uint32_t base = 10;
  if (allow_hex && len - i > 2 && p[i] == '0' && (p[i + 1] == 'x' || p[i + 1] == 'X')) {
    base = 16;
    i += 2;
  }
  if (i == len) return false;
  int64_t ndig = len - i;
  if (base == 10 && ndig <= 18) {
    int64_t v = 0;
    for (; i < len; i++) {
      if (p[i] < '0' || p[i] > '9') return false;
      v = v * 10 + (p[i] - '0');
    }
    *out = sd_int_from_i64(sign * v);
    return true;
  }
  if (base == 16) {
    /* Eight hex digits per limb, from the end. */
    sd_bigint *r = big_alloc(ndig / 8 + 1);
    int32_t n = 0;
    uint32_t limb = 0;
    int shift = 0;
    for (int64_t j = len - 1; j >= i; j--) {
      char c = p[j];
      uint32_t d;
      if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
      else {
        sd_obj_free(r);
        return false;
      }
      limb |= d << shift;
      shift += 4;
      if (shift == 32) {
        r->d[n++] = limb;
        limb = 0;
        shift = 0;
      }
    }
    if (shift) r->d[n++] = limb;
    *out = big_finish(r, sign, n);
    return true;
  }
  for (int64_t j = i; j < len; j++)
    if (p[j] < '0' || p[j] > '9') return false;
  sd_bigint *r = big_alloc(ndig / 9 + 2);
  int32_t n;
  if (!fast || ndig <= TEXT_IN_LEAF) {
    n = from_dec_basecase(p + i, ndig, r->d);
  } else {
    pow10_t pw[40];
    int count = 0;
    pow10_next(pw, count++);
    while (((int64_t)9 << count) < ndig) pow10_next(pw, count++);
    n = from_dec(p + i, ndig, r->d, pw, count - 1);
    pow10_free(pw, count);
  }
  *out = big_finish(r, sign, n);
  return true;
}

static bool parse_int(const char *p, int64_t len, bool allow_hex, sd_int *out) {
  return parse_int_by(p, len, allow_hex, out, true);
}

bool sd_int_parse_dec_basecase_(const char *p, int64_t len, sd_int *out) {
  return parse_int_by(p, len, false, out, false);
}

bool sd_int_parse_dec_(const char *p, int64_t len, sd_int *out) { return parse_int(p, len, false, out); }

sd_int sd_int_from_lit(const char *text) {
  sd_int v;
  if (!parse_int(text, (int64_t)strlen(text), true, &v)) sd_fatal("invalid integer literal");
  return v;
}

sd_int sd_int_biglit_(void *cache, const char *text) {
  uintptr_t *c = cache;
  uintptr_t s = __atomic_load_n(c, __ATOMIC_ACQUIRE);
  if (s) return (sd_int){(int64_t)(s - 1)};
  sd_int v = sd_int_from_lit(text);
  if (v.w & 1) sd_int_big_(v)->rc = SD_RC_IMMORTAL;
  uintptr_t want = (uintptr_t)v.w + 1, expected = 0;
  if (!__atomic_compare_exchange_n(c, &expected, want, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    if (v.w & 1) sd_obj_free(sd_int_big_(v));
    return (sd_int){(int64_t)(expected - 1)};
  }
  return v;
}
