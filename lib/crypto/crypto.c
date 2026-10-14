/* Natives of package crypto (runtime/README.md, "Native functions"):
 * SHA-256 (FIPS 180-4), HMAC-SHA-256 (RFC 2104), OS randomness and a
 * constant-time comparison. */
#include "seed.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

/* ---------------------------------------------------------------- SHA-256 */

typedef struct {
  uint32_t h[8];
  uint64_t total; /* bytes hashed so far */
  uint8_t block[64];
  size_t used; /* bytes in block */
} crypto_sha256_ctx;

static const uint32_t crypto_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define CRYPTO_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void crypto_sha256_init(crypto_sha256_ctx *c) {
  static const uint32_t h0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(c->h, h0, sizeof h0);
  c->total = 0;
  c->used = 0;
}

static void crypto_sha256_block(crypto_sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = CRYPTO_ROTR(w[i - 15], 7) ^ CRYPTO_ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = CRYPTO_ROTR(w[i - 2], 17) ^ CRYPTO_ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = CRYPTO_ROTR(e, 6) ^ CRYPTO_ROTR(e, 11) ^ CRYPTO_ROTR(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + S1 + ch + crypto_sha256_k[i] + w[i];
    uint32_t S0 = CRYPTO_ROTR(a, 2) ^ CRYPTO_ROTR(a, 13) ^ CRYPTO_ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

static void crypto_sha256_update(crypto_sha256_ctx *c, const uint8_t *p, size_t n) {
  c->total += n;
  if (c->used > 0) {
    size_t take = 64 - c->used < n ? 64 - c->used : n;
    memcpy(c->block + c->used, p, take);
    c->used += take;
    p += take;
    n -= take;
    if (c->used < 64) return;
    crypto_sha256_block(c, c->block);
    c->used = 0;
  }
  for (; n >= 64; p += 64, n -= 64) crypto_sha256_block(c, p);
  memcpy(c->block, p, n);
  c->used = n;
}

static void crypto_sha256_final(crypto_sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->total * 8;
  uint8_t pad[72] = {0x80};
  size_t padlen = (c->used < 56 ? 56 : 120) - c->used;
  for (int i = 0; i < 8; i++) pad[padlen + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
  crypto_sha256_update(c, pad, padlen + 8);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
}

sd_str sd_lib_crypto_sha256(sd_str data) {
  crypto_sha256_ctx c;
  uint8_t out[32];
  crypto_sha256_init(&c);
  crypto_sha256_update(&c, (const uint8_t *)data->data, (size_t)data->len);
  crypto_sha256_final(&c, out);
  return sd_str_new((const char *)out, 32);
}

sd_str sd_lib_crypto_hmacSha256(sd_str key, sd_str data) {
  uint8_t k[64] = {0}, pad[64], inner[32], out[32];
  crypto_sha256_ctx c;
  if (key->len > 64) {
    crypto_sha256_init(&c);
    crypto_sha256_update(&c, (const uint8_t *)key->data, (size_t)key->len);
    crypto_sha256_final(&c, k);
  } else {
    memcpy(k, key->data, (size_t)key->len);
  }
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
  crypto_sha256_init(&c);
  crypto_sha256_update(&c, pad, 64);
  crypto_sha256_update(&c, (const uint8_t *)data->data, (size_t)data->len);
  crypto_sha256_final(&c, inner);
  for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
  crypto_sha256_init(&c);
  crypto_sha256_update(&c, pad, 64);
  crypto_sha256_update(&c, inner, 32);
  crypto_sha256_final(&c, out);
  return sd_str_new((const char *)out, 32);
}

/* ------------------------------------------------------------- randomness */

/* Fills buf with n bytes from the OS: getentropy (at most 256 bytes per
 * call), or /dev/urandom where getentropy is missing or fails. Returns 0, or
 * the errno of the failure. (lib/rand/rand.c has its own copy for seeding:
 * a package's C helpers are static to it.) */
static int crypto_os_random(uint8_t *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    size_t chunk = n - done > 256 ? 256 : n - done;
    if (getentropy(buf + done, chunk) != 0) break;
    done += chunk;
  }
  if (done == n) return 0;
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return errno;
  while (done < n) {
    ssize_t r = read(fd, buf + done, n - done);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) {
      int e = r < 0 ? errno : EIO;
      close(fd);
      return e;
    }
    done += (size_t)r;
  }
  close(fd);
  return 0;
}

sd_str sd_lib_crypto_randomBytes(sd_int n) {
  int64_t len;
  if (!sd_int_to_i64(n, &len) || len < 0) {
    sd_str s = sd_int_to_str(n);
    if (s->data[0] == '-') sd_crash("crypto.randomBytes: n (%s) must not be negative", s->data);
    sd_crash("crypto.randomBytes: n (%s) is too large", s->data);
  }
  if (len == 0) return sd_str_from_cstr("");
  uint8_t *buf = sd_alloc((size_t)len);
  int e = crypto_os_random(buf, (size_t)len);
  if (e != 0) {
    free(buf);
    sd_crash("crypto.randomBytes: the OS provided no randomness: %s", sd_strerror(e));
  }
  sd_str out = sd_str_new((const char *)buf, len);
  free(buf);
  return out;
}

/* The loop always runs over every byte, and the accumulator is volatile, so
 * the compiler cannot stop at the first difference. */
bool sd_lib_crypto_equal(sd_str a, sd_str b) {
  if (a->len != b->len) return false;
  volatile uint8_t diff = 0;
  const volatile uint8_t *x = (const volatile uint8_t *)a->data, *y = (const volatile uint8_t *)b->data;
  for (int64_t i = 0; i < a->len; i++) diff |= x[i] ^ y[i];
  return diff == 0;
}
