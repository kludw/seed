/* A tiny test harness for the runtime's C unit tests. */
#ifndef SEED_CHECK_H
#define SEED_CHECK_H

#include "seed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_failures, check_count;

#define CHECK(cond) do { \
    check_count++; \
    if (!(cond)) { \
      check_failures++; \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

/* Compare a C string with an expected one, printing both on failure. */
#define CHECK_STREQ(got, want) do { \
    const char *g_ = (got), *w_ = (want); \
    check_count++; \
    if (strcmp(g_, w_) != 0) { \
      check_failures++; \
      fprintf(stderr, "%s:%d: CHECK_STREQ failed: %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #got, g_, w_); \
    } \
  } while (0)

/* Owned sd_str -> compared and released. */
#define CHECK_SDSTR(s, want) do { \
    sd_str s_ = (s); \
    CHECK_STREQ(sd_str_cstr(s_), want); \
    sd_str_release(s_); \
  } while (0)

/* An owned sd_int compared with decimal text, then released. */
#define CHECK_INT(v, want) do { \
    sd_int v_ = (v); \
    sd_str t_ = sd_int_to_str(v_); \
    CHECK_STREQ(sd_str_cstr(t_), want); \
    sd_str_release(t_); \
    sd_int_release(v_); \
  } while (0)

static int check_done(const char *name) {
  if (check_failures) {
    fprintf(stderr, "%s: %d of %d checks FAILED\n", name, check_failures, check_count);
    return 1;
  }
  printf("%s: all %d checks passed\n", name, check_count);
  return 0;
}

#endif
