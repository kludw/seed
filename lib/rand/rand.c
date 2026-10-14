/* Natives of package rand (runtime/README.md, "Native functions"): the
 * per-goroutine generator behind rand.range, rand.fraction and
 * rand.permutation. Each goroutine is an OS thread, so a thread-local
 * SplitMix64 state is a generator per goroutine. It is seeded from the OS
 * (getentropy, or /dev/urandom) the first time the goroutine uses it. */
#include "seed.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif

static _Thread_local bool rand_seeded;
static _Thread_local uint64_t rand_state;

/* Threads are reused by later goroutines (runtime/README.md, section 10):
 * the next goroutine on this thread seeds its own generator. */
static void rand_forget(void) { rand_seeded = false; }

static void rand_seed_from_os(void) {
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
  rand_state = seed;
  rand_seeded = true;
  sd_goroutine_atexit(rand_forget);
}

sd_int sd_lib_rand_nextOs(void) {
  if (!rand_seeded) rand_seed_from_os();
  uint64_t z = (rand_state += 0x9e3779b97f4a7c15u);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
  return sd_int_from_u64(z ^ (z >> 31));
}
