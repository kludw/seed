/* Natives of package rand (runtime/README.md, "Native functions"): the
 * per-goroutine generator behind rand.range, rand.fraction and
 * rand.permutation: a SplitMix64 state in goroutine-local storage (a
 * goroutine is not tied to a thread), seeded from the OS (getentropy, or
 * /dev/urandom) the first time the goroutine uses it. */
#include "seed.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

typedef struct {
  bool seeded;
  uint64_t state;
} rand_gen;

static int rand_key;

static void rand_seed_from_os(rand_gen *g) {
  uint64_t seed;
  if (getentropy(&seed, sizeof seed) != 0) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    ssize_t r = -1;
    if (fd >= 0) {
      do r = read(fd, &seed, sizeof seed);
      while (r < 0 && errno == EINTR);
      close(fd);
    }
    if (r != (ssize_t)sizeof seed) sd_crash("rand: the OS provided no randomness to seed the generator");
  }
  g->state = seed;
  g->seeded = true;
}

sd_int sd_lib_rand_nextOs(void) {
  rand_gen *g = sd_goroutine_local(&rand_key, sizeof(rand_gen));
  if (!g->seeded) rand_seed_from_os(g);
  uint64_t z = (g->state += 0x9e3779b97f4a7c15u);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
  return sd_int_from_u64(z ^ (z >> 31));
}
