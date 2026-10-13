/* The whole runtime as one translation unit:
 *   cc -std=c11 -O2 -I runtime prog.c runtime/seed_all.c -lpthread -lm
 * (or compile the seed_*.c files separately; they are independent units). */
#include "seed_core.c"
#include "seed_int.c"
#include "seed_str.c"
#include "seed_list.c"
#include "seed_task.c"
#include "seed_pkg.c"
#include "seed_test.c"
