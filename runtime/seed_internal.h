/* Declarations shared by the runtime's implementation files. Not for
 * generated code. */
#ifndef SEED_INTERNAL_H
#define SEED_INTERNAL_H

#include "seed.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(SD_COUNT_OBJECTS)
#include <stdatomic.h>
extern _Atomic int64_t sd_live_count_;
#define SD_COUNT_ALLOC() atomic_fetch_add_explicit(&sd_live_count_, 1, memory_order_relaxed)
#define SD_COUNT_FREE() atomic_fetch_sub_explicit(&sd_live_count_, 1, memory_order_relaxed)
#else
#define SD_COUNT_ALLOC() ((void)0)
#define SD_COUNT_FREE() ((void)0)
#endif

/* Counted objects (strings, lists, maps, boxes, bigints, channels) are
 * allocated and freed with these, so that SD_COUNT_OBJECTS can count them. */
SD_INLINE void *sd_obj_alloc(size_t n) {
  SD_COUNT_ALLOC();
  return sd_alloc(n);
}
SD_INLINE void sd_obj_free(void *p) {
  SD_COUNT_FREE();
  free(p);
}

SD_NORETURN void sd_fatal(const char *msg); /* out of memory and other unrecoverable faults */

/* Formats into a malloc'ed C string. */
char *sd_vfmt(const char *fmt, va_list ap);
char *sd_fmt(const char *fmt, ...) SD_PRINTF(1, 2);
/* sd_crash with a message already formatted (malloc'ed; taken over), so
 * that the text of a number in it can be freed first: a crash must not leak
 * (the cleanup frames release everything else). */
SD_NORETURN void sd_crash_owned(char *msg);

/* Names the current goroutine in the deadlock report (name: a static
 * string, or one that lives as long as the goroutine). */
void sd_task_set_name_(const char *name);

/* Internal string helpers. */
sd_str sd_str_alloc_(int64_t len); /* uninitialised data of len bytes, NUL written */
sd_str sd_str_byte_(uint8_t c);    /* immortal one-byte string */
char *sd_int_to_cstr(sd_int a);    /* malloc'ed decimal text */
sd_bigint *sd_bigint_copy_(const sd_bigint *b);

/* Lists */
sd_list *sd_list_new_len_(const sd_type *et, int64_t len); /* len elements, uninitialised */

/* Freeing without recursion (seed_core.c): sd_box_free_, sd_list_free_ and
 * sd_map_free_ queue the object; the outermost call frees the queue. */
enum { SD_DEAD_BOX = 1, SD_DEAD_LIST = 2, SD_DEAD_MAP = 3 };
void sd_free_queued_(void *obj, int kind);
void sd_list_destroy_(sd_list *l);
void sd_map_destroy_(sd_map *m);

/* Bytes needed for one value of type t, rounded up to 8. */
SD_INLINE size_t sd_round8(size_t n) { return (n + 7) & ~(size_t)7; }

#endif
