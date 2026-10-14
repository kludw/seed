/* Natives of package time (runtime/README.md, "Native functions"). */
#include "seed.h"

#include <time.h>

sd_chan *sd_lib_time_after(sd_int d) { return sd_time_after(d); }

void sd_lib_time_sleep(sd_int d) { sd_time_sleep(d); }

sd_int sd_lib_time_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return sd_int_from_i64((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec);
}

/* The runtime's clock (CLOCK_MONOTONIC), the one timers use. */
sd_int sd_lib_time_monotonic(void) { return sd_time_now(); }
