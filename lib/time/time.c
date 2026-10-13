/* Natives of package time (runtime/README.md, "Native functions"). */
#include "seed.h"

sd_chan *sd_lib_time_after(sd_int d) { return sd_time_after(d); }

void sd_lib_time_sleep(sd_int d) { sd_time_sleep(d); }
