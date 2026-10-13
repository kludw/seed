/* Natives of package os (runtime/README.md, "Native functions"). */
#include "seed.h"

void sd_lib_os_exit(sd_int code) { sd_os_exit(code); }

void sd_lib_os_crash(sd_str message) { sd_os_crash(message); }

sd_str sd_lib_os_env(sd_str name) { return sd_os_env(name); }

sd_err sd_lib_os_run(sd_list *argv, sd_int *code) { return sd_os_run(argv, code); }

sd_str sd_lib_os_executable(void) { return sd_os_executable(); }

sd_err sd_lib_os_cwd(sd_str *out) { return sd_os_cwd(out); }
