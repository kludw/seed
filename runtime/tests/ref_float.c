/* The reference float formatter for test_str: the bootstrap compiler
 * seed0's sd_float_str (seed0/runtime.c, removed once the compiler was
 * self-hosting), kept here unchanged apart from writing into a buffer.
 * Shortest text that reads back as the same value (like Python's repr):
 * fixed notation when -4 <= exponent < 16, otherwise d.ddde+XX. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ref_float_str(double x, char *res, int cap) {
  char buf[64], digits[32], out[64];
  int n = 0;
  if (isnan(x)) {
    n = snprintf(out, sizeof out, "nan");
  } else if (isinf(x)) {
    n = snprintf(out, sizeof out, "%s", x > 0 ? "inf" : "-inf");
  } else {
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
  }
  if (n > cap - 1) n = cap - 1;
  memcpy(res, out, (size_t)n);
  res[n] = 0;
  return n;
}
