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
static void mag_divmod(uint32_t *q, uint32_t *r, const uint32_t *u, int32_t m, const uint32_t *v, int32_t n) {
  const uint64_t b = (uint64_t)1 << 32;
  uint32_t *un = sd_alloc((size_t)(m + 1) * sizeof(uint32_t));
  uint32_t *vn = sd_alloc((size_t)n * sizeof(uint32_t));
  int s = __builtin_clz(v[n - 1]);
  for (int32_t i = n - 1; i > 0; i--) vn[i] = (v[i] << s) | (uint32_t)((uint64_t)v[i - 1] >> (32 - s));
  vn[0] = v[0] << s;
  un[m] = (uint32_t)((uint64_t)u[m - 1] >> (32 - s));
  for (int32_t i = m - 1; i > 0; i--) un[i] = (u[i] << s) | (uint32_t)((uint64_t)u[i - 1] >> (32 - s));
  un[0] = u[0] << s;
  for (int32_t j = m - n; j >= 0; j--) {
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
  if (r)
    for (int32_t i = 0; i < n; i++) r[i] = (un[i] >> s) | (uint32_t)((uint64_t)un[i + 1] << (32 - s));
  free(un);
  free(vn);
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

sd_int sd_int_mul_slow(sd_int a, sd_int b) {
  bview x, y;
  bv_init(&x, a);
  bv_init(&y, b);
  if (x.n == 0 || y.n == 0) return SD_INT(0);
  sd_bigint *r = big_alloc((int64_t)x.n + y.n);
  mag_mul(r->d, x.d, x.n, y.d, y.n);
  return big_finish(r, x.sign * y.sign, x.n + y.n);
}

void sd_crash_div_zero(void) { sd_crash("division by zero"); }

/* Truncating division: q = trunc(a / b), r = a - q * b. */
static void int_divmod(sd_int a, sd_int b, sd_int *q, sd_int *r) {
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
  mag_divmod(qq->d, rr ? rr->d : NULL, x.d, x.n, y.d, y.n);
  if (q) *q = big_finish(qq, x.sign * y.sign, x.n - y.n + 1);
  else sd_obj_free(qq);
  if (r) *r = big_finish(rr, x.sign, y.n);
}

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
    sd_str s = sd_float_to_str(f);
    sd_crash("cannot convert float %s to int", sd_str_cstr(s));
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
  sd_crash("cannot convert %s to byte", s);
}

/* ----------------------------------------------------------- text */

char *sd_int_to_cstr(sd_int a) {
  if ((a.w & 1) == 0) return sd_fmt("%lld", (long long)(a.w >> 1));
  if (a.w == 1) return sd_fmt("null");
  bview x;
  bv_init(&x, a);
  int32_t n = x.n;
  uint32_t *t = sd_alloc((size_t)n * sizeof(uint32_t));
  memcpy(t, x.d, (size_t)n * sizeof(uint32_t));
  size_t maxchunks = (size_t)n * 32 / 29 + 2; /* 10^9 > 2^29 */
  uint32_t *chunks = sd_alloc(maxchunks * sizeof(uint32_t));
  size_t nc = 0;
  while (n > 0) {
    chunks[nc++] = mag_divsmall(t, t, n, 1000000000u);
    while (n > 0 && t[n - 1] == 0) n--;
  }
  char *s = sd_alloc(nc * 9 + 3);
  char *p = s;
  if (x.sign < 0) *p++ = '-';
  p += sprintf(p, "%u", chunks[nc - 1]);
  for (size_t i = nc - 1; i-- > 0;) p += sprintf(p, "%09u", chunks[i]);
  free(t);
  free(chunks);
  return s;
}

/* Parses [-]digits (base 10) or [-]0x hexdigits (when allow_hex). */
static bool parse_int(const char *p, int64_t len, bool allow_hex, sd_int *out) {
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
  int32_t n = 0;
  int64_t cap = ndig / 2 + 2; /* more than enough limbs: each limb holds >= 8 hex or 9 decimal digits */
  sd_bigint *r = big_alloc(cap);
  uint32_t chunk = 0, mul = 1;
  int per = base == 10 ? 9 : 7;
  int inchunk = 0;
  for (; i < len; i++) {
    char c = p[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (base == 16 && c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (base == 16 && c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else {
      sd_obj_free(r);
      return false;
    }
    chunk = chunk * base + d;
    mul *= base;
    if (++inchunk == per) {
      mag_mul_small_add(r->d, &n, mul, chunk);
      chunk = 0;
      mul = 1;
      inchunk = 0;
    }
  }
  if (inchunk) mag_mul_small_add(r->d, &n, mul, chunk);
  *out = big_finish(r, sign, n);
  return true;
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
