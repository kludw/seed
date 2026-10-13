/* Wraps seed0's float formatting so test_str can check that the new runtime
 * prints every float exactly like seed0 (separate translation unit: seed0's
 * runtime uses the same sd_ names). */
#include "../../seed0/runtime.c"

int seed0_float_str(double x, char *buf, int cap) {
  sd_str s = sd_float_str(x);
  int n = (int)s.len < cap - 1 ? (int)s.len : cap - 1;
  memcpy(buf, s.p, (size_t)n);
  buf[n] = 0;
  return n;
}
