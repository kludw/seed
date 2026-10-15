/* Strings, text conversions and float formatting. */
#include "check.h"

#include <math.h>

int ref_float_str(double x, char *buf, int cap); /* ref_float.c: seed0's formatter */

static sd_str S(const char *s) { return sd_str_from_cstr(s); }

static sd_err expect_crash(sd_err e, const char *substr) {
  check_count++;
  if (sd_err_is_null(e)) {
    check_failures++;
    fprintf(stderr, "expected crash containing \"%s\"\n", substr);
    return e;
  }
  sd_str m = sd_err_message(e);
  if (!strstr(sd_str_cstr(m), substr)) {
    check_failures++;
    fprintf(stderr, "crash \"%s\" lacks \"%s\"\n", sd_str_cstr(m), substr);
  }
  sd_str_release(m);
  sd_err_release(e);
  return SD_ERR_NULL;
}

static void test_float_format(void) {
  struct { double x; const char *s; } cases[] = {
    {1.0, "1.0"}, {0.1, "0.1"}, {1e300, "1e+300"}, {-1e300, "-1e+300"}, {0.0, "0.0"}, {-0.0, "-0.0"},
    {1e16, "1e+16"}, {1e15, "1000000000000000.0"}, {1234.5, "1234.5"}, {0.0001, "0.0001"},
    {0.00001, "1e-05"}, {3.14, "3.14"}, {1.5e-7, "1.5e-07"}, {2500.0, "2500.0"}, {0.25, "0.25"},
    {123456789012345680.0, "1.2345678901234568e+17"}, {5e-324, "5e-324"}, {1.7976931348623157e308, "1.7976931348623157e+308"},
    {100000.0, "100000.0"}, {0.3, "0.3"}, {2.0 / 3.0, "0.6666666666666666"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) CHECK_SDSTR(sd_float_to_str(cases[i].x), cases[i].s);
  CHECK_SDSTR(sd_float_to_str(NAN), "nan");
  CHECK_SDSTR(sd_float_to_str(INFINITY), "inf");
  CHECK_SDSTR(sd_float_to_str(-INFINITY), "-inf");
  /* identical to seed0's formatter (ref_float.c) for many random doubles */
  uint64_t r = 0x12345678abcdefULL;
  char want[64];
  for (int i = 0; i < 20000; i++) {
    r ^= r << 13;
    r ^= r >> 7;
    r ^= r << 17;
    double x;
    if (i % 2) {
      uint64_t bits = r;
      memcpy(&x, &bits, sizeof x);
      if (isnan(x)) continue;
    } else {
      x = (double)(int64_t)(r % 2000001) / pow(10.0, (double)(r % 9)) - 1000.0;
    }
    ref_float_str(x, want, sizeof want);
    CHECK_SDSTR(sd_float_to_str(x), want);
    /* and it reads back as the same value */
    sd_str s = sd_float_to_str(x);
    double back;
    sd_err e = sd_str_to_float(s, &back);
    CHECK(sd_err_is_null(e) && back == x);
    sd_str_release(s);
  }
  /* the neighbours of every power of two and ten, where the rounding
   * interval is uneven or the digits are few: Ryu, the exact fast path and
   * the printf loop (which decides the rare cases) must agree */
  double edges[3];
  for (int k = -1074; k <= 1023; k += 3) {
    edges[0] = ldexp(1.0, k);
    edges[1] = nextafter(edges[0], 0.0);
    edges[2] = nextafter(edges[0], INFINITY);
    for (int j = 0; j < 3; j++) {
      ref_float_str(edges[j], want, sizeof want);
      CHECK_SDSTR(sd_float_to_str(edges[j]), want);
    }
  }
  for (int k = -323; k <= 308; k += 2) {
    char lit[16];
    snprintf(lit, sizeof lit, "1e%d", k);
    edges[0] = strtod(lit, NULL);
    edges[1] = nextafter(edges[0], 0.0);
    edges[2] = nextafter(edges[0], INFINITY);
    for (int j = 0; j < 3; j++) {
      ref_float_str(-edges[j], want, sizeof want);
      CHECK_SDSTR(sd_float_to_str(-edges[j]), want);
    }
  }
  /* exact short decimals (the fast path without Ryu) and their neighbours */
  for (int i = 0; i < 3000; i++) {
    double x = (double)(i * 7919 % 100003) / 8.0 + (double)(i % 3) * 1e9;
    ref_float_str(x, want, sizeof want);
    CHECK_SDSTR(sd_float_to_str(x), want);
    x = nextafter(x, INFINITY);
    ref_float_str(x, want, sizeof want);
    CHECK_SDSTR(sd_float_to_str(x), want);
  }
}

/* sd_str_to_float is strtod's value bit for bit, fast path or not, and
 * rejects what it always rejected. */
static void test_float_parse(void) {
  const char *texts[] = {"0", "-0", "+0", "0.0", "-0.0", ".5", "5.", "1e5", "1E+5", "1e-5", "2.5e3", "4498.5",
                         "0.1", "0.3", "123456.789", "-1.5e-3", "1e22", "1e23", "1e-22", "1e-23", "9007199254740992",
                         "9007199254740993", "9007199254740991e15", "1234567890123456789", "12345678901234567890",
                         "0.000000000000000000000000000001", "123456789012345678901234567890", "1e400", "1e-400",
                         "4.35679e+37", "2.2250738585072011e-308", "1.7976931348623157e308", "5e-324", "00012.50",
                         "1.00000000000000011102230246251565404236316680908203125", "inf", "-infinity", "nan",
                         "0x1p3", "0x10", "1e99999999999999999999", "1e-99999999999999999999"};
  for (size_t i = 0; i < sizeof texts / sizeof texts[0]; i++) {
    double d = 1, want = strtod(texts[i], NULL);
    sd_str s = S(texts[i]);
    sd_err e = sd_str_to_float(s, &d);
    CHECK(sd_err_is_null(e) && memcmp(&d, &want, sizeof d) == 0);
    if (!sd_err_is_null(e)) {
      fprintf(stderr, "toFloat(%s) failed\n", texts[i]);
      sd_err_release(e);
    }
    sd_str_release(s);
  }
  const char *bad[] = {"", ".", "-", "+", "e5", "1e", "1e+", "1.5x", "1..5", "--1", " 1", "1 ", "\t1", "0x"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    double d = 1;
    sd_str s = S(bad[i]);
    sd_err e = sd_str_to_float(s, &d);
    CHECK(!sd_err_is_null(e) && d == 0);
    if (!sd_err_is_null(e)) sd_err_release(e);
    sd_str_release(s);
  }
  /* random decimal texts */
  uint64_t r = 0x9e3779b97f4a7c15ULL;
  char buf[48];
  for (int i = 0; i < 20000; i++) {
    r ^= r << 13;
    r ^= r >> 7;
    r ^= r << 17;
    int n = 0, digits = 1 + (int)(r % 20), dot = (int)((r >> 8) % 24);
    if ((r >> 16) & 1) buf[n++] = '-';
    for (int j = 0; j < digits; j++) {
      if (j == dot) buf[n++] = '.';
      buf[n++] = (char)('0' + (r >> (20 + j * 2)) % 10);
    }
    if ((r >> 60) & 1) n += snprintf(buf + n, sizeof buf - (size_t)n, "e%d", (int)((r >> 32) % 70) - 35);
    buf[n] = 0;
    double d, want = strtod(buf, NULL);
    sd_str s = S(buf);
    sd_err e = sd_str_to_float(s, &d);
    CHECK(sd_err_is_null(e) && memcmp(&d, &want, sizeof d) == 0);
    if (!sd_err_is_null(e)) sd_err_release(e);
    sd_str_release(s);
  }
}

static void test_scalars_to_str(void) {
  CHECK_SDSTR(sd_int_to_str(SD_INT(-42)), "-42");
  CHECK_SDSTR(sd_int_to_str(SD_INT(0)), "0");
  CHECK_SDSTR(sd_bool_to_str(true), "true");
  CHECK_SDSTR(sd_bool_to_str(false), "false");
  CHECK_SDSTR(sd_byte_to_str('a'), "97");
  CHECK_SDSTR(sd_byte_to_str(255), "255");
}

static sd_err crash_at(void *p) {
  (void)sd_str_at(*(sd_str *)p, SD_INT(12));
  return SD_ERR_NULL;
}
static sd_err crash_slice(void *p) {
  sd_str r = sd_str_slice(*(sd_str *)p, SD_INT(2), SD_INT(1));
  sd_str_release(r);
  return SD_ERR_NULL;
}
static sd_err crash_split(void *p) {
  sd_list *l = sd_str_split(*(sd_str *)p, SD_STR_LIT(""));
  sd_list_release(l);
  return SD_ERR_NULL;
}
static sd_err crash_replace(void *p) {
  sd_str r = sd_str_replace(*(sd_str *)p, SD_STR_LIT(""), SD_STR_LIT("x"));
  sd_str_release(r);
  return SD_ERR_NULL;
}

static void check_list(sd_list *l, const char *joined) {
  sd_str j = sd_list_join(l, SD_STR_LIT("|"));
  CHECK_STREQ(sd_str_cstr(j), joined);
  sd_str_release(j);
  sd_list_release(l);
}

static void test_methods(void) {
  sd_str s = S("hello, world");
  CHECK(sd_int_eq(sd_str_len(s), SD_INT(12)));
  CHECK(sd_str_at(s, SD_INT(1)) == 'e');
  CHECK_SDSTR(sd_str_slice(s, SD_INT(7), SD_INT(12)), "world");
  CHECK_SDSTR(sd_str_slice(s, SD_INT(0), SD_INT(0)), "");
  CHECK(sd_str_contains(s, SD_STR_LIT("lo, w")));
  CHECK(!sd_str_contains(s, SD_STR_LIT("xyz")));
  CHECK(sd_int_eq(sd_str_index(s, SD_STR_LIT("o")), SD_INT(4)));
  CHECK(sd_int_is_null(sd_str_index(s, SD_STR_LIT("z"))));
  CHECK(sd_str_starts_with(s, SD_STR_LIT("hell")));
  CHECK(!sd_str_starts_with(s, SD_STR_LIT("world")));
  CHECK(sd_str_ends_with(s, SD_STR_LIT("world")));
  CHECK(sd_str_ends_with(s, SD_STR_LIT("")));
  CHECK_SDSTR(sd_str_to_upper(s), "HELLO, WORLD");
  CHECK_SDSTR(sd_str_to_lower(SD_STR_LIT("MiXeD 123")), "mixed 123");
  CHECK_SDSTR(sd_str_replace(s, SD_STR_LIT("o"), SD_STR_LIT("0")), "hell0, w0rld");
  CHECK_SDSTR(sd_str_replace(s, SD_STR_LIT("hello, world"), SD_STR_LIT("")), "");
  CHECK_SDSTR(sd_str_replace(s, SD_STR_LIT("l"), SD_STR_LIT("LL")), "heLLLLo, worLLd");
  CHECK_SDSTR(sd_str_trim(SD_STR_LIT(" \t\r\n x y \n")), "x y");
  CHECK_SDSTR(sd_str_trim(SD_STR_LIT("   ")), "");
  check_list(sd_str_split(SD_STR_LIT("a,b,"), SD_STR_LIT(",")), "a|b|");
  check_list(sd_str_split(SD_STR_LIT("a,b,"), SD_STR_LIT(",")), "a|b|");
  sd_list *parts = sd_str_split(SD_STR_LIT("a,b,"), SD_STR_LIT(","));
  CHECK(parts->len == 3);
  sd_list_release(parts);
  check_list(sd_str_split(SD_STR_LIT(""), SD_STR_LIT(",")), "");
  check_list(sd_str_split(SD_STR_LIT("a::b::c"), SD_STR_LIT("::")), "a|b|c");
  /* chars: tests/full/chars_utf8.sd */
  sd_str word = S("h\xc3\xa9llo");
  sd_list *cs = sd_str_chars(word);
  CHECK(cs->len == 5);
  CHECK(sd_str_len_raw(SD_LIST_GET(cs, sd_str, SD_INT(1))) == 2);
  sd_list_reverse(&cs);
  CHECK_SDSTR(sd_list_join(cs, SD_STR_LIT("")), "oll\xc3\xa9h");
  sd_list_release(cs);
  sd_str_release(word);
  sd_str emoji = S("a\xf0\x9f\x98\x80" "b");
  cs = sd_str_chars(emoji);
  CHECK(cs->len == 3);
  sd_list_release(cs);
  sd_str_release(emoji);
  sd_str badutf = S("a\xff\xc3");
  cs = sd_str_chars(badutf);
  CHECK(cs->len == 3);
  sd_list_release(cs);
  sd_str_release(badutf);
  /* bytes */
  sd_list *bs = sd_str_bytes(SD_STR_LIT("AB"));
  CHECK(bs->len == 2 && SD_LIST_GET(bs, uint8_t, SD_INT(1)) == 'B');
  uint8_t c = 'C';
  sd_list_push(&bs, &c);
  CHECK_SDSTR(sd_str_from_bytes(bs), "ABC");
  sd_list_release(bs);
  /* comparisons */
  CHECK(sd_str_cmp(SD_STR_LIT("abc"), SD_STR_LIT("abd")) < 0);
  CHECK(sd_str_cmp(SD_STR_LIT("ab"), SD_STR_LIT("abc")) < 0);
  CHECK(sd_str_cmp(SD_STR_LIT("b"), SD_STR_LIT("abc")) > 0);
  CHECK(sd_str_eq(SD_STR_LIT("x"), SD_STR_LIT("x")));
  sd_str x1 = S("same"), x2 = S("same");
  CHECK(x1 != x2 && sd_str_eq(x1, x2) && sd_str_hash(x1) == sd_str_hash(x2));
  CHECK(!sd_str_eq(x1, NULL) && sd_str_eq(NULL, NULL));
  sd_str_release(x1);
  sd_str_release(x2);
  /* toFloat */
  double d;
  sd_err e = sd_str_to_float(SD_STR_LIT("2.5e3"), &d);
  CHECK(sd_err_is_null(e) && d == 2500.0);
  e = sd_str_to_float(SD_STR_LIT("2.5x"), &d);
  CHECK(!sd_err_is_null(e));
  sd_str m = sd_err_message(e);
  CHECK_STREQ(sd_str_cstr(m), "string.toFloat: \"2.5x\": invalid float");
  sd_str_release(m);
  sd_err_release(e);
  e = sd_str_to_float(SD_STR_LIT(" 1"), &d);
  CHECK(!sd_err_is_null(e));
  sd_err_release(e);
  /* crashes */
  expect_crash(sd_catch(crash_at, &s), "index out of range: 12 with length 12");
  expect_crash(sd_catch(crash_slice, &s), "slice bounds out of range: [2:1] with length 12");
  expect_crash(sd_catch(crash_split, &s), "empty separator");
  expect_crash(sd_catch(crash_replace, &s), "empty string");
  sd_str_release(s);
}

static void test_concat(void) {
  sd_str a = S("foo"), b = S("bar");
  CHECK_SDSTR(sd_str_concat(a, b), "foobar");
  CHECK_SDSTR(sd_str_concat(a, SD_STR_LIT("")), "foo");
  sd_str parts[] = {a, SD_STR_LIT("-"), b, SD_STR_LIT("-"), a};
  CHECK_SDSTR(sd_str_concat_n(parts, 5), "foo-bar-foo");
  /* append: in place when unshared, copy when shared */
  sd_str acc = sd_str_retain(a);
  acc = sd_str_append(acc, b); /* a is shared: copies, a unchanged */
  CHECK_STREQ(sd_str_cstr(a), "foo");
  CHECK_STREQ(sd_str_cstr(acc), "foobar");
  for (int i = 0; i < 10000; i++) acc = sd_str_append(acc, SD_STR_LIT("x"));
  CHECK(acc->len == 10006 && acc->cap >= acc->len && acc->data[acc->len] == 0);
  sd_str_release(acc);
  /* append_parts: strings and ints written in place, or into a copy */
  acc = sd_str_append_parts(S("n="), (sd_str_part[]){{NULL, SD_INT(-42)}, {SD_STR_LIT(",")}, {b}}, 3);
  CHECK_STREQ(sd_str_cstr(acc), "n=-42,bar");
  sd_str shared = sd_str_retain(acc);
  acc = sd_str_append_parts(acc, (sd_str_part[]){{SD_STR_LIT("!")}}, 1); /* shared: copies */
  CHECK_STREQ(sd_str_cstr(shared), "n=-42,bar");
  CHECK_STREQ(sd_str_cstr(acc), "n=-42,bar!");
  sd_str_release(shared);
  acc = sd_str_append_parts(acc, (sd_str_part[]){{acc}, {SD_STR_LIT("|")}, {acc}}, 3); /* a part is acc itself */
  CHECK_STREQ(sd_str_cstr(acc), "n=-42,bar!n=-42,bar!|n=-42,bar!");
  sd_int big = sd_int_shl(SD_INT(1), SD_INT(100));
  acc = sd_str_append_parts(acc, (sd_str_part[]){{SD_STR_LIT(" ")}, {NULL, big}, {SD_STR_LIT("")}}, 3);
  CHECK(acc->len > 31 && strcmp(acc->data + acc->len - 31, "1267650600228229401496703205376") == 0);
  sd_int_release(big);
  acc = sd_str_append_parts(acc, (sd_str_part[]){{SD_STR_LIT("")}}, 1); /* nothing to add */
  for (int i = 0; i < 3000; i++) acc = sd_str_append_parts(acc, (sd_str_part[]){{SD_STR_LIT("ab")}, {NULL, SD_INT(i % 10)}}, 2);
  CHECK(acc->len == 63 + 3000 * 3 && acc->data[acc->len] == 0 && acc->data[acc->len - 1] == '9');
  sd_str_release(acc);
  acc = sd_str_append_parts(SD_STR_LIT(""), (sd_str_part[]){{a}}, 1); /* an immortal start */
  CHECK_STREQ(sd_str_cstr(acc), "foo");
  sd_str_release(acc);
  acc = sd_str_append(sd_str_retain(a), a); /* b is a itself */
  CHECK_STREQ(sd_str_cstr(acc), "foofoo");
  sd_str_release(acc);
  /* literals are immortal */
  sd_str lit = SD_STR_LIT("literal");
  CHECK(lit->rc == SD_RC_IMMORTAL);
  sd_str_release(sd_str_retain(lit));
  CHECK(lit->rc == SD_RC_IMMORTAL);
  sd_str_release(a);
  sd_str_release(b);
}

int main(void) {
  int64_t live0 = sd_live_objects();
  test_float_format();
  test_scalars_to_str();
  test_concat();
  test_float_parse();
  int64_t live1 = sd_live_objects();
  CHECK(live1 == live0);
  test_methods();
  return check_done("test_str");
}
