/* Natives of package fmt (runtime/README.md, "Native functions"). */
#include "seed.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void sd_lib_fmt_print(sd_str s) { sd_fmt_print(s); }

void sd_lib_fmt_println(sd_str s) { sd_fmt_println(s); }

void sd_lib_fmt_eprint(sd_str s) { sd_fmt_eprint(s); }

void sd_lib_fmt_eprintln(sd_str s) { sd_fmt_eprintln(s); }

/* The C library's printf is correctly rounded (round-half-even on the exact
 * binary value) on macOS and glibc. nan is spelled out so that no "-nan"
 * appears. */
sd_str sd_lib_fmt_fixed(double f, sd_int places) {
  int64_t p;
  if (!sd_int_to_i64(places, &p) || p < 0 || p > 100) {
    sd_crash_int("fmt.fixed: places (%s) must be between 0 and 100", places);
  }
  if (isnan(f)) return sd_str_from_cstr("nan");
  if (isinf(f)) return sd_str_from_cstr(f > 0 ? "inf" : "-inf");
  int n = snprintf(NULL, 0, "%.*f", (int)p, f);
  char *buf = sd_alloc((size_t)n + 1);
  snprintf(buf, (size_t)n + 1, "%.*f", (int)p, f);
  sd_str out = sd_str_new(buf, n);
  free(buf);
  return out;
}
