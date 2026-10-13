/* Natives of package fmt (runtime/README.md, "Native functions"). */
#include "seed.h"

void sd_lib_fmt_print(sd_str s) { sd_fmt_print(s); }

void sd_lib_fmt_println(sd_str s) { sd_fmt_println(s); }

void sd_lib_fmt_eprint(sd_str s) { sd_fmt_eprint(s); }

void sd_lib_fmt_eprintln(sd_str s) { sd_fmt_eprintln(s); }
