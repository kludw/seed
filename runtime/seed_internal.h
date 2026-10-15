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

/* The small-object allocator (seed_core.c; runtime/README.md, section 15):
 * size classes up to 2 KB with per-thread caches over a central pool, malloc
 * above that. Memory from sd_mem_alloc is freed with sd_mem_free and resized
 * with sd_mem_realloc only, never with free/realloc (and the other way
 * round). Payloads are 16-byte aligned.
 *
 * Under sanitizers (and with SD_PLAIN_MALLOC, which tests/leaks.sh defines)
 * these are plain malloc/realloc/free, so that ASan, TSan, LeakSanitizer and
 * macOS leaks see every object. */
#if !defined(SD_PLAIN_MALLOC)
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(SD_LEAK_CHECK)
#define SD_PLAIN_MALLOC 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(leak_sanitizer) || \
    __has_feature(memory_sanitizer)
#define SD_PLAIN_MALLOC 1
#endif
#endif
#endif

#if defined(SD_PLAIN_MALLOC)
SD_INLINE void *sd_mem_alloc(size_t n) { return sd_alloc(n); }
SD_INLINE void sd_mem_free(void *p) { free(p); }
SD_INLINE void *sd_mem_realloc(void *p, size_t n) { return sd_realloc(p, n); }
SD_INLINE size_t sd_mem_usable(const void *p, size_t n) { return (void)p, n; }
#else
/* Out of line on purpose: each call finds the calling thread's cache
 * itself, so no caller can keep a thread-local address across a point where
 * its goroutine may move to another thread (README section 15). */
__attribute__((noinline)) void *sd_mem_alloc(size_t n);
__attribute__((noinline)) void sd_mem_free(void *p);
void *sd_mem_realloc(void *p, size_t n);
/* The bytes usable at p, which was allocated (or resized) for n bytes:
 * at least n, the whole size class for a small object. */
size_t sd_mem_usable(const void *p, size_t n);
#endif

/* Counted objects (strings, lists, maps, boxes, bigints, channels) are
 * allocated and freed with these, so that SD_COUNT_OBJECTS can count them. */
SD_INLINE void *sd_obj_alloc(size_t n) {
  SD_COUNT_ALLOC();
  return sd_mem_alloc(n);
}
SD_INLINE void sd_obj_free(void *p) {
  SD_COUNT_FREE();
  sd_mem_free(p);
}
SD_INLINE void *sd_obj_realloc(void *p, size_t n) { return sd_mem_realloc(p, n); }

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
/* The schoolbook (quadratic) big integer algorithms, which the tests compare
 * with the fast ones that seed_int.c uses for long numbers. */
sd_int sd_int_mul_basecase_(sd_int a, sd_int b);
void sd_int_divmod_basecase_(sd_int a, sd_int b, sd_int *q, sd_int *r);
char *sd_int_to_cstr_basecase_(sd_int a);
bool sd_int_parse_dec_basecase_(const char *p, int64_t len, sd_int *out);

/* Lists */
sd_list *sd_list_new_len_(const sd_type *et, int64_t len); /* len elements, uninitialised */

/* Freeing without recursion (seed_core.c): sd_box_free_, sd_list_free_ and
 * sd_map_free_ queue the object; the outermost call frees the queue. */
enum { SD_DEAD_BOX = 1, SD_DEAD_LIST = 2, SD_DEAD_MAP = 3 };
void sd_free_queued_(void *obj, int kind);
void sd_free_queue_reset_(void);
void sd_list_destroy_(sd_list *l);
void sd_map_destroy_(sd_map *m);

/* Bytes needed for one value of type t, rounded up to 8. */
SD_INLINE size_t sd_round8(size_t n) { return (n + 7) & ~(size_t)7; }

#endif
