/* Growing integers: known values, Python-generated vectors, differential
 * tests against __int128, algebraic identities, the fast algorithms for long
 * numbers against the schoolbook ones, and crash behaviour. */
#include "check.h"
#include "int_vectors.h"

static sd_int P(const char *s) { return sd_int_from_lit(s); }

static void check_crash(sd_err e, const char *want_substr, int line) {
  check_count++;
  if (sd_err_is_null(e)) {
    check_failures++;
    fprintf(stderr, "test_int.c:%d: expected a crash containing \"%s\", got none\n", line, want_substr);
    return;
  }
  sd_str m = sd_err_message(e);
  if (!strstr(sd_str_cstr(m), want_substr)) {
    check_failures++;
    fprintf(stderr, "test_int.c:%d: crash \"%s\" does not contain \"%s\"\n", line, sd_str_cstr(m), want_substr);
  }
  sd_str_release(m);
  sd_err_release(e);
}
#define CHECK_CRASH(e, s) check_crash((e), (s), __LINE__)

static void test_known(void) {
  sd_int f = SD_INT(1);
  for (int i = 2; i <= 30; i++) {
    sd_int n = sd_int_mul(f, SD_INT(i));
    sd_int_release(f);
    f = n;
  }
  CHECK_INT(f, "265252859812191058636308480000000");

  sd_int p = SD_INT(1);
  for (int i = 0; i < 100; i++) {
    sd_int n = sd_int_mul(p, SD_INT(2));
    sd_int_release(p);
    p = n;
  }
  sd_int p2 = sd_int_shl(SD_INT(1), SD_INT(100));
  CHECK(sd_int_eq(p, p2));
  CHECK_INT(p2, "1267650600228229401496703205376");

  /* tests/full/bigint_arith.sd */
  sd_int a = P("123456789012345678901234567890"), b = P("987654321098765432109876543210");
  CHECK_INT(sd_int_mul(a, b), "121932631137021795226185032733622923332237463801111263526900");
  CHECK_INT(sd_int_sub(b, a), "864197532086419753208641975320");
  CHECK_INT(sd_int_sub(a, b), "-864197532086419753208641975320");
  CHECK_INT(sd_int_div(p, SD_INT(3)), "422550200076076467165567735125");
  CHECK_INT(sd_int_mod(p, SD_INT(1000007)), "698635");
  sd_int np = sd_int_neg(p);
  CHECK_INT(sd_int_div(np, SD_INT(7)), "-181092942889747057356671886482");
  CHECK_INT(sd_int_mod(np, SD_INT(7)), "-2");
  CHECK(sd_int_lt(a, b));
  sd_int ab = sd_int_mul(a, b), ba = sd_int_mul(b, a);
  CHECK(sd_int_eq(ab, ba));
  sd_int_release(ab);
  sd_int_release(ba);

  sd_int x = SD_INT(0), y = SD_INT(1);
  for (int i = 0; i < 100; i++) {
    sd_int nx = sd_int_add(x, y);
    sd_int_release(x);
    x = y;
    y = nx;
  }
  CHECK_INT(x, "354224848179261915075");
  sd_int_release(y);
  sd_int_release(np);
  sd_int_release(p);
  sd_int_release(a);
  sd_int_release(b);
}

static void test_vectors(void) {
  for (size_t i = 0; i < sizeof int_vectors / sizeof int_vectors[0]; i++) {
    sd_int a = P(int_vectors[i].a), b = P(int_vectors[i].b);
    CHECK_INT(sd_int_add(a, b), int_vectors[i].add);
    CHECK_INT(sd_int_sub(a, b), int_vectors[i].sub);
    CHECK_INT(sd_int_mul(a, b), int_vectors[i].mul);
    if (int_vectors[i].div[0]) {
      CHECK_INT(sd_int_div(a, b), int_vectors[i].div);
      CHECK_INT(sd_int_mod(a, b), int_vectors[i].mod);
    }
    CHECK_INT(sd_int_and(a, b), int_vectors[i].and_);
    CHECK_INT(sd_int_or(a, b), int_vectors[i].or_);
    CHECK_INT(sd_int_xor(a, b), int_vectors[i].xor_);
    CHECK(sd_int_cmp(a, b) == int_vectors[i].cmp);
    CHECK(sd_int_eq(a, b) == (int_vectors[i].cmp == 0));
    CHECK(sd_int_lt(a, b) == (int_vectors[i].cmp < 0));
    CHECK(sd_int_ge(a, b) == (int_vectors[i].cmp >= 0));
    /* parse(to_string(a)) round trip and hash agreement */
    sd_str s = sd_int_to_str(a);
    sd_int back;
    sd_err e = sd_str_to_int(s, &back);
    CHECK(sd_err_is_null(e));
    CHECK(sd_int_eq(back, a));
    CHECK(sd_int_hash(back) == sd_int_hash(a));
    sd_int_release(back);
    sd_str_release(s);
    sd_int_release(a);
    sd_int_release(b);
  }
  for (size_t i = 0; i < sizeof shift_vectors / sizeof shift_vectors[0]; i++) {
    sd_int a = P(shift_vectors[i].a);
    CHECK_INT(sd_int_shl(a, SD_INT(shift_vectors[i].k)), shift_vectors[i].shl);
    CHECK_INT(sd_int_shr(a, SD_INT(shift_vectors[i].k)), shift_vectors[i].shr);
    sd_int_release(a);
  }
  for (size_t i = 0; i < sizeof float_vectors / sizeof float_vectors[0]; i++) {
    sd_int a = P(float_vectors[i].a);
    double f = sd_int_to_float(a);
    CHECK_SDSTR(sd_float_to_str(f), float_vectors[i].f);
    /* int(float(a)) == a whenever the float is exact */
    sd_int back = sd_float_to_int(f);
    CHECK(sd_int_to_float(back) == f);
    sd_int_release(back);
    sd_int_release(a);
  }
}

static void i128_str(__int128 v, char *buf) {
  char tmp[64];
  int n = 0;
  bool neg = v < 0;
  unsigned __int128 m = neg ? -(unsigned __int128)v : (unsigned __int128)v;
  do {
    tmp[n++] = (char)('0' + (int)(m % 10));
    m /= 10;
  } while (m);
  int k = 0;
  if (neg) buf[k++] = '-';
  while (n) buf[k++] = tmp[--n];
  buf[k] = 0;
}

static uint64_t rng_state = 88172645463325252ULL;
static uint64_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}
static int64_t rnd_i64(void) {
  int bits = (int)(rnd() % 64) + 1;
  uint64_t v = rnd() >> (64 - bits);
  return (rnd() & 1) ? -(int64_t)(v >> 1) : (int64_t)(v >> 1);
}

static void test_differential(void) {
  char want[64];
  for (int i = 0; i < 200000; i++) {
    int64_t x = rnd_i64(), y = rnd_i64();
    if (i % 7 == 0) x = (int64_t)(rnd() | 1) >> (rnd() % 3);
    sd_int a = sd_int_from_i64(x), b = sd_int_from_i64(y);
    int64_t back;
    CHECK(sd_int_to_i64(a, &back) && back == x);
    __int128 X = x, Y = y;
    i128_str(X + Y, want);
    CHECK_INT(sd_int_add(a, b), want);
    i128_str(X - Y, want);
    CHECK_INT(sd_int_sub(a, b), want);
    i128_str(X * Y, want);
    CHECK_INT(sd_int_mul(a, b), want);
    if (y != 0) {
      i128_str(X / Y, want);
      CHECK_INT(sd_int_div(a, b), want);
      i128_str(X % Y, want);
      CHECK_INT(sd_int_mod(a, b), want);
    }
    i128_str(X & Y, want);
    CHECK_INT(sd_int_and(a, b), want);
    i128_str(X | Y, want);
    CHECK_INT(sd_int_or(a, b), want);
    i128_str(X ^ Y, want);
    CHECK_INT(sd_int_xor(a, b), want);
    i128_str(-X, want);
    CHECK_INT(sd_int_neg(a), want);
    int k = (int)(rnd() % 64);
    i128_str(X >> k, want);
    CHECK_INT(sd_int_shr(a, SD_INT(k)), want);
    if (x > -((int64_t)1 << 62) && x < ((int64_t)1 << 62)) {
      i128_str((__int128)((unsigned __int128)X << k), want);
      CHECK_INT(sd_int_shl(a, SD_INT(k)), want);
    }
    CHECK(sd_int_cmp(a, b) == ((x > y) - (x < y)));
    CHECK(sd_int_to_float(a) == (double)x);
    sd_int_release(a);
    sd_int_release(b);
  }
}

static sd_int rnd_big(void) {
  int limbs = (int)(rnd() % 12) + 1;
  sd_int v = SD_INT(0);
  for (int i = 0; i < limbs; i++) {
    sd_int s = sd_int_shl(v, SD_INT(32));
    sd_int_release(v);
    v = sd_int_or(s, SD_INT(rnd() & 0xffffffffu));
    sd_int_release(s);
  }
  if (rnd() & 1) {
    sd_int n = sd_int_neg(v);
    sd_int_release(v);
    v = n;
  }
  return v;
}

static void test_identities(void) {
  for (int i = 0; i < 3000; i++) {
    sd_int a = rnd_big(), b = rnd_big();
    if (sd_int_eq(b, SD_INT(0))) {
      sd_int_release(b);
      b = SD_INT(7);
    }
    sd_int q = sd_int_div(a, b), r = sd_int_mod(a, b);
    sd_int qb = sd_int_mul(q, b), back = sd_int_add(qb, r);
    CHECK(sd_int_eq(back, a)); /* a == (a/b)*b + a%b */
    CHECK(sd_int_cmp(sd_int_and(r, SD_INT(0)), SD_INT(0)) == 0);
    sd_int ab = sd_int_mul(a, b), abb = sd_int_div(ab, b);
    CHECK(sd_int_eq(abb, a)); /* (a*b)/b == a */
    sd_int x = sd_int_xor(a, b), o = sd_int_or(a, b), n = sd_int_and(a, b), on = sd_int_sub(o, n);
    CHECK(sd_int_eq(x, on)); /* a^b == (a|b) - (a&b) */
    sd_int s = sd_int_shl(a, SD_INT(77)), s2 = sd_int_shr(s, SD_INT(77));
    CHECK(sd_int_eq(s2, a));
    sd_int *all[] = {&a, &b, &q, &r, &qb, &back, &ab, &abb, &x, &o, &n, &on, &s, &s2};
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) sd_int_release(*all[k]);
  }
}

/* ---- the subquadratic algorithms against the schoolbook ones ----
 *
 * Long products use Karatsuba (on 64-bit words), long divisions
 * Burnikel-Ziegler, and long text conversions divide and conquer; each is
 * compared with the schoolbook version that seed_int.c keeps for short
 * numbers, on random operands of sizes across the thresholds, in patterns
 * that stress carries and borrows. */
sd_int sd_int_mul_basecase_(sd_int a, sd_int b);
void sd_int_divmod_basecase_(sd_int a, sd_int b, sd_int *q, sd_int *r);
char *sd_int_to_cstr(sd_int a);
char *sd_int_to_cstr_basecase_(sd_int a);
bool sd_int_parse_dec_(const char *p, int64_t len, sd_int *out);
bool sd_int_parse_dec_basecase_(const char *p, int64_t len, sd_int *out);

/* A number of n 32-bit limbs (0 for zero) in one of several patterns. */
static sd_int rnd_limbs(int n, int kind) {
  if (n == 0) return SD_INT(0);
  char *s = malloc((size_t)n * 8 + 8);
  int p = 0;
  if (rnd() & 1) s[p++] = '-';
  s[p++] = '0';
  s[p++] = 'x';
  for (int i = 0; i < n; i++) {
    uint32_t limb;
    switch (kind) {
    case 0: limb = 0xFFFFFFFFu; break;                                     /* 2^k - 1 */
    case 1: limb = i == 0 ? 1 : 0; break;                                   /* 2^k */
    case 2: limb = rnd() % 4 == 0 ? (uint32_t)rnd() : 0; break;             /* sparse */
    case 3: limb = rnd() % 4 == 0 ? 0xFFFFFFFFu : (uint32_t)rnd(); break;   /* mostly random, runs of ones */
    case 4: limb = i == 0 ? 0x80000000u : (uint32_t)(rnd() & 0xF); break;   /* top bit set, small limbs */
    default: limb = (uint32_t)rnd(); break;
    }
    if (i == 0 && limb == 0) limb = 1 + (uint32_t)(rnd() % 7);
    p += sprintf(s + p, i == 0 ? "%x" : "%08x", limb);
  }
  s[p] = 0;
  sd_int v = sd_int_from_lit(s);
  free(s);
  return v;
}

static int rnd_size(int max) {
  switch (rnd() % 5) {
  case 0: return (int)(rnd() % 4);
  case 1: return 20 + (int)(rnd() % 120); /* around the thresholds */
  default: return (int)(rnd() % (uint64_t)(max + 1));
  }
}

static void check_text(sd_int v) {
  char *fast = sd_int_to_cstr(v), *slow = sd_int_to_cstr_basecase_(v);
  CHECK_STREQ(fast, slow);
  /* back, with leading zeros sometimes */
  size_t len = strlen(fast);
  char *t = malloc(len + 64);
  int off = 0, neg = fast[0] == '-';
  if (neg) t[off++] = '-';
  int zeros = rnd() % 4 == 0 ? (int)(rnd() % 40) : 0;
  memset(t + off, '0', (size_t)zeros);
  strcpy(t + off + zeros, fast + neg);
  sd_int p1, p2;
  CHECK(sd_int_parse_dec_(t, (int64_t)strlen(t), &p1));
  CHECK(sd_int_parse_dec_basecase_(t, (int64_t)strlen(t), &p2));
  CHECK(sd_int_eq(p1, v));
  CHECK(sd_int_eq(p2, v));
  sd_int_release(p1);
  sd_int_release(p2);
  free(t);
  free(fast);
  free(slow);
}

static void check_divmod(sd_int a, sd_int b) {
  if (sd_int_eq(b, SD_INT(0))) return;
  sd_int q1 = sd_int_div(a, b), r1 = sd_int_mod(a, b), q2, r2;
  sd_int_divmod_basecase_(a, b, &q2, &r2);
  CHECK(sd_int_eq(q1, q2));
  CHECK(sd_int_eq(r1, r2));
  sd_int_release(q1), sd_int_release(r1), sd_int_release(q2), sd_int_release(r2);
}

static void test_fast_algorithms(void) {
  for (int i = 0; i < 400; i++) {
    sd_int x = rnd_limbs(rnd_size(900), (int)(rnd() % 8)), y = rnd_limbs(rnd_size(900), (int)(rnd() % 8));
    if (rnd() % 6 == 0) { /* operands of very different sizes */
      sd_int_release(y);
      y = rnd_limbs(1 + (int)(rnd() % 3), (int)(rnd() % 8));
    }
    sd_int m1 = sd_int_mul(x, y), m2 = sd_int_mul_basecase_(x, y);
    CHECK(sd_int_eq(m1, m2));
    sd_int s1 = sd_int_mul(x, x), s2 = sd_int_mul_basecase_(x, x);
    CHECK(sd_int_eq(s1, s2));
    sd_int m1x = sd_int_add(m1, x);
    check_divmod(m1x, y); /* a long quotient and a long divisor */
    check_divmod(s1, y);
    check_divmod(x, y);
    check_text(i % 2 ? m1 : x);
    sd_int *all[] = {&x, &y, &m1, &m2, &s1, &s2, &m1x};
    for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) sd_int_release(*all[k]);
  }
  /* Powers of ten and their neighbours, around the conversion's split
   * points (10^(9 * 2^k)): 10^n, 10^n - 1, 10^n + 1, negated. */
  sd_int ten = SD_INT(1);
  for (int n = 0; n <= 9 * 512 + 3; n++) {
    bool edge = n < 40 || n % 9 < 2 || n % 9 == 8 || n % 97 == 0;
    if (edge) {
      sd_int below = sd_int_sub(ten, SD_INT(1)), above = sd_int_add(ten, SD_INT(1)), neg = sd_int_neg(ten);
      check_text(ten);
      check_text(below);
      check_text(above);
      check_text(neg);
      sd_int *all[] = {&below, &above, &neg};
      for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) sd_int_release(*all[k]);
    }
    sd_int t = sd_int_mul(ten, SD_INT(10));
    sd_int_release(ten);
    ten = t;
  }
  /* The 4609-digit power of ten exactly, divided by its square root and by
   * nearby numbers. */
  char *s = sd_int_to_cstr(ten);
  CHECK(s[0] == '1' && strlen(s) == 9 * 512 + 5 && strspn(s + 1, "0") == 9 * 512 + 4);
  free(s);
  sd_int big = sd_int_mul(ten, ten), less = sd_int_sub(ten, SD_INT(1)), negbig = sd_int_neg(big);
  check_divmod(big, ten);
  check_divmod(big, less);
  check_divmod(negbig, less);
  check_text(big);
  sd_int_release(negbig);
  sd_int_release(less);
  sd_int_release(big);
  sd_int_release(ten);
  /* Limb boundaries: 2^(32k) - 1, 2^(32k), 2^(32k) + 1 squared and divided. */
  for (int k = 1; k < 200; k += (k < 70 ? 1 : 37)) {
    sd_int p = sd_int_shl(SD_INT(1), SD_INT(32 * k));
    sd_int v[3] = {sd_int_sub(p, SD_INT(1)), sd_int_retain(p), sd_int_add(p, SD_INT(1))};
    for (int a = 0; a < 3; a++) {
      for (int b = 0; b < 3; b++) {
        sd_int m1 = sd_int_mul(v[a], v[b]), m2 = sd_int_mul_basecase_(v[a], v[b]);
        CHECK(sd_int_eq(m1, m2));
        check_divmod(m1, v[b]);
        sd_int m1b = sd_int_sub(m1, SD_INT(1));
        check_divmod(m1b, v[a]);
        sd_int_release(m1), sd_int_release(m2), sd_int_release(m1b);
      }
      check_text(v[a]);
    }
    for (int a = 0; a < 3; a++) sd_int_release(v[a]);
    sd_int_release(p);
  }
}

static sd_err crash_div0(void *p) {
  sd_int r = sd_int_div(*(sd_int *)p, SD_INT(0));
  sd_int_release(r);
  return SD_ERR_NULL;
}
static sd_err crash_mod0(void *p) {
  sd_int r = sd_int_mod(*(sd_int *)p, SD_INT(0));
  sd_int_release(r);
  return SD_ERR_NULL;
}
static sd_err crash_byte(void *p) {
  (void)sd_int_to_byte(*(sd_int *)p);
  return SD_ERR_NULL;
}
static sd_err crash_neg_shift(void *p) {
  sd_int r = sd_int_shl(*(sd_int *)p, SD_INT(-1));
  sd_int_release(r);
  return SD_ERR_NULL;
}
static sd_err crash_float(void *p) {
  sd_int r = sd_float_to_int(*(double *)p);
  sd_int_release(r);
  return SD_ERR_NULL;
}

static void test_crashes(void) {
  sd_int small = SD_INT(5), big = P("100000000000000000000000");
  CHECK_CRASH(sd_catch(crash_div0, &small), "division by zero");
  CHECK_CRASH(sd_catch(crash_div0, &big), "division by zero");
  CHECK_CRASH(sd_catch(crash_mod0, &big), "division by zero");
  sd_int b300 = SD_INT(300), bneg = SD_INT(-1);
  CHECK_CRASH(sd_catch(crash_byte, &b300), "cannot convert 300 to byte");
  CHECK_CRASH(sd_catch(crash_byte, &bneg), "cannot convert -1 to byte");
  CHECK_CRASH(sd_catch(crash_byte, &big), "to byte");
  CHECK_CRASH(sd_catch(crash_neg_shift, &small), "negative shift count");
  CHECK_CRASH(sd_catch(crash_neg_shift, &big), "negative shift count");
  double nan_ = 0.0 / 0.0, inf_ = 1.0 / 0.0;
  CHECK_CRASH(sd_catch(crash_float, &nan_), "cannot convert float nan to int");
  CHECK_CRASH(sd_catch(crash_float, &inf_), "cannot convert float inf to int");
  CHECK(sd_int_to_byte(SD_INT(255)) == 255);
  CHECK(sd_int_to_byte(SD_INT(0)) == 0);
  sd_int_release(big);
}

static void test_misc(void) {
  /* small range boundaries */
  CHECK_INT(sd_int_add(SD_INT(SD_INT_SMALL_MAX), SD_INT(1)), "4611686018427387904");
  CHECK_INT(sd_int_sub(SD_INT(SD_INT_SMALL_MIN), SD_INT(1)), "-4611686018427387905");
  CHECK_INT(sd_int_neg(SD_INT(SD_INT_SMALL_MIN)), "4611686018427387904");
  CHECK_INT(sd_int_div(SD_INT(SD_INT_SMALL_MIN), SD_INT(-1)), "4611686018427387904");
  CHECK_INT(sd_int_mod(SD_INT(SD_INT_SMALL_MIN), SD_INT(-1)), "0");
  CHECK_INT(sd_int_from_i64(INT64_MIN), "-9223372036854775808");
  CHECK_INT(sd_int_from_u64(UINT64_MAX), "18446744073709551615");
  CHECK_INT(sd_int_mod(SD_INT(-7), SD_INT(2)), "-1");
  CHECK_INT(sd_int_div(SD_INT(-7), SD_INT(2)), "-3");
  CHECK_INT(sd_int_shr(SD_INT(-5), SD_INT(1)), "-3");
  CHECK_INT(sd_int_shr(SD_INT(-5), SD_INT(1000)), "-1");
  CHECK_INT(sd_int_shl(SD_INT(0), SD_INT(1000000)), "0");
  CHECK_INT(sd_int_from_lit("0xff"), "255");
  CHECK_INT(sd_int_from_lit("-0x10000000000000000"), "-18446744073709551616");
  /* a normalised result: big - big that fits is small again */
  sd_int a = P("100000000000000000000001"), b = P("100000000000000000000000");
  sd_int d = sd_int_sub(a, b);
  CHECK(sd_int_is_small(d) && sd_int_eq(d, SD_INT(1)));
  sd_int_release(a);
  sd_int_release(b);
  /* conversions */
  CHECK(sd_int_to_float(SD_INT(3)) == 3.0);
  CHECK_INT(sd_float_to_int(-2.9), "-2");
  CHECK_INT(sd_float_to_int(1e20), "100000000000000000000");
  CHECK_INT(sd_float_to_int(-1e30), "-1000000000000000019884624838656");
  int64_t out;
  sd_int huge = P("9223372036854775808");
  CHECK(!sd_int_to_i64(huge, &out));
  sd_int_release(huge);
  /* parse errors */
  const char *bad[] = {"", "-", "+5", "12a", " 1", "1 ", "0x10", "--1"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    sd_str s = sd_str_from_cstr(bad[i]);
    sd_int v;
    sd_err e = sd_str_to_int(s, &v);
    CHECK(!sd_err_is_null(e));
    CHECK(sd_int_is_null(v));
    sd_err_release(e);
    sd_str_release(s);
  }
  sd_str ok = SD_STR_LIT("-000123");
  sd_int v;
  sd_err e = sd_str_to_int(ok, &v);
  CHECK(sd_err_is_null(e));
  CHECK_INT(v, "-123");
  /* cached big literal: immortal, same object every time */
  sd_int l1 = SD_INT(0), l2 = SD_INT(1);
  for (int i = 0; i < 2; i++) {
    sd_int l = SD_INT_BIGLIT("123456789012345678901234567890");
    if (i == 0) l1 = l;
    else l2 = l;
  }
  CHECK(l1.w == l2.w);
  CHECK(sd_int_big_(l1)->rc == SD_RC_IMMORTAL);
  sd_int_release(l1); /* harmless */
  CHECK_INT(sd_int_retain(l1), "123456789012345678901234567890");
  /* index */
  CHECK(sd_int_index(SD_INT(2), 3) == 2);
}

int main(void) {
  test_misc(); /* leaves one immortal cached literal */
  int64_t live0 = sd_live_objects();
  test_known();
  test_vectors();
  test_differential();
  test_identities();
  test_fast_algorithms();
  int64_t live1 = sd_live_objects();
  CHECK(live1 == live0); /* every bigint freed (only checked with -DSD_COUNT_OBJECTS) */
  test_crashes();
  return check_done("test_int");
}
