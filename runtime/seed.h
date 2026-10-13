/* seed.h - the Seed runtime API.
 *
 * Generated C programs include this header first (before any other header),
 * and are linked with the runtime implementation (seed_all.c, or the
 * separate seed_*.c files). runtime/README.md is the reference for the
 * representation of every Seed type, the ownership rules and how statements
 * are lowered; the comments here are a summary.
 *
 * Ownership in one paragraph:
 *   - Arguments are BORROWED: the callee neither keeps nor releases them.
 *   - Results are OWNED (+1): the caller must release them (or move them).
 *   - Exceptions are named: functions that STORE a value into a container
 *     (push, insert, set, map set, box new, iface new) CONSUME that value
 *     (they take ownership of the bytes at `val`). Accessors that return a
 *     pointer into a container (sd_list_at_mut, sd_map_get, sd_box_data, ...)
 *     return BORROWED pointers that are valid until the container changes.
 *   - A function taking `T **` (a place) may replace *place (copy-on-write).
 */
#ifndef SEED_H
#define SEED_H

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE 1
#endif
#if !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SD_INLINE static inline __attribute__((unused))
#define SD_NORETURN __attribute__((noreturn))
#define SD_LIKELY(x) __builtin_expect(!!(x), 1)
#define SD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define SD_PRINTF(a, b) __attribute__((format(printf, a, b)))

/* Reference count value of objects that are never freed and never counted
 * (string literals, static enum cases, cached big literals). Such objects
 * may be shared between goroutines. */
#define SD_RC_IMMORTAL (-1)

/* ======================================================================
 * Types
 * ====================================================================== */

typedef struct sd_int { int64_t w; } sd_int;  /* tagged word, see below */
typedef struct sd_bigint sd_bigint;
typedef struct sd_string sd_string;
typedef sd_string *sd_str;                     /* string; NULL = null string? */
typedef struct sd_list sd_list;                /* []T;    NULL = null []T?    */
typedef struct sd_map sd_map;                  /* map;    NULL = null map?    */
typedef struct sd_box sd_box;                  /* box;    NULL = null         */
typedef struct sd_chan sd_chan;                /* chan T; NULL = null chan?   */
typedef struct sd_type sd_type;

/* Interface values. vt == NULL means null. obj is a box holding the
 * dynamic value; its type (obj->t) identifies the dynamic type. */
typedef struct sd_vtable { const char *type_name; } sd_vtable;
typedef struct sd_iface { const sd_vtable *vt; sd_box *obj; } sd_iface;
typedef sd_iface sd_err;                       /* error; vt == NULL = no error */

/* The vtable layout every `error` implementation must use. */
typedef struct sd_error_vtable {
  sd_vtable hdr;
  sd_str (*message)(const void *self);         /* self: borrowed payload; returns owned */
} sd_error_vtable;

/* Nullable scalars that have no spare bit pattern. (int? is an sd_int that
 * may hold SD_INT_NULL; pointer types use NULL.) */
typedef struct { bool has; double v; } sd_opt_float;
typedef struct { bool has; bool v; } sd_opt_bool;
typedef struct { bool has; uint8_t v; } sd_opt_byte;

/* ---- type descriptors ----
 * Containers, channels and spawn need to retain, release, deep-copy and
 * compare values of any type. Every type is described by an sd_type. The
 * runtime defines descriptors for the built-in types; generated code
 * defines one per struct, enum and spawn-argument struct, normally as a
 * field table (SD_K_STRUCT / SD_K_ENUM), so it needs no code. */
typedef enum sd_kind {
  SD_K_FLOAT = 1, /* double */
  SD_K_BOOL,      /* bool */
  SD_K_BYTE,      /* uint8_t */
  SD_K_FN,        /* any C function pointer */
  SD_K_INT,       /* sd_int (also int?) */
  SD_K_STR,       /* sd_str (also string?) */
  SD_K_LIST,      /* sd_list* (also []T?) */
  SD_K_MAP,       /* sd_map* */
  SD_K_BOX,       /* sd_box* (nullable structs, enums, recursive types) */
  SD_K_CHAN,      /* sd_chan* */
  SD_K_IFACE,     /* sd_iface: equal when same dynamic type and equal payload */
  SD_K_ERR,       /* sd_err: equal when both null or same message() */
  SD_K_OPT_FLOAT, /* sd_opt_float */
  SD_K_OPT_BOOL,  /* sd_opt_bool */
  SD_K_OPT_BYTE,  /* sd_opt_byte */
  SD_K_STRUCT,    /* C struct described by `fields` */
  SD_K_ENUM,      /* C struct starting with int32_t tag, described by `variants` */
  SD_K_CUSTOM     /* hand-written functions below */
} sd_kind;

typedef struct sd_field {
  size_t offset;         /* offsetof(struct, field) */
  const sd_type *type;
} sd_field;

typedef struct sd_variant {
  const char *name;
  const sd_field *fields; /* offsets relative to the start of the enum struct */
  int32_t nfields;
} sd_variant;

struct sd_type {
  const char *name;
  size_t size;            /* sizeof the C representation */
  sd_kind kind;
  bool trivial;           /* true: no counted references inside (plain copy, no-op release) */
  const sd_field *fields; /* SD_K_STRUCT */
  int32_t nfields;
  const sd_variant *variants; /* SD_K_ENUM; the tag (int32_t at offset 0) indexes this */
  int32_t nvariants;
  /* SD_K_CUSTOM only (all four required): */
  void (*retain)(void *p);
  void (*release)(void *p);
  void (*clone_deep)(void *dst, const void *src);
  bool (*equal)(const void *a, const void *b);
};

/* Descriptor initialisers for generated code. */
#define SD_STRUCT_TYPE(NAME, CTYPE, FIELDS) \
  { .name = NAME, .size = sizeof(CTYPE), .kind = SD_K_STRUCT, \
    .fields = FIELDS, .nfields = (int32_t)(sizeof(FIELDS) / sizeof((FIELDS)[0])) }
#define SD_STRUCT_TYPE_TRIVIAL(NAME, CTYPE, FIELDS) \
  { .name = NAME, .size = sizeof(CTYPE), .kind = SD_K_STRUCT, .trivial = true, \
    .fields = FIELDS, .nfields = (int32_t)(sizeof(FIELDS) / sizeof((FIELDS)[0])) }
#define SD_ENUM_TYPE(NAME, CTYPE, VARIANTS) \
  { .name = NAME, .size = sizeof(CTYPE), .kind = SD_K_ENUM, \
    .variants = VARIANTS, .nvariants = (int32_t)(sizeof(VARIANTS) / sizeof((VARIANTS)[0])) }
#define SD_FIELD(CTYPE, MEMBER, TYPE) { offsetof(CTYPE, MEMBER), TYPE }

/* Built-in descriptors. */
extern const sd_type sd_type_int, sd_type_float, sd_type_bool, sd_type_byte, sd_type_fn;
extern const sd_type sd_type_str, sd_type_list, sd_type_map, sd_type_box, sd_type_chan;
extern const sd_type sd_type_iface, sd_type_err;
extern const sd_type sd_type_opt_float, sd_type_opt_bool, sd_type_opt_byte;

/* Generic operations on a value of type t stored at p. */
void sd_retain(const sd_type *t, void *p);
void sd_release(const sd_type *t, void *p);
void sd_clone_deep(const sd_type *t, void *dst, const void *src); /* dst gets an owned, unshared copy */
bool sd_equal(const sd_type *t, const void *a, const void *b);
int sd_compare(const sd_type *t, const void *a, const void *b);   /* int, float, byte, string */
uint64_t sd_hash(const sd_type *t, const void *p);                /* int, byte, bool, string */
bool sd_type_is_trivial(const sd_type *t);

/* ======================================================================
 * Crashes and memory
 * ====================================================================== */

/* Crash the current goroutine: runs its pending defers, then the crash
 * becomes the group's error (or, in main, "crash: <msg>" and exit 2). */
SD_NORETURN void sd_crash(const char *fmt, ...) SD_PRINTF(1, 2);
SD_NORETURN void sd_crash_index(sd_int i, int64_t len);
SD_NORETURN void sd_crash_null(void); /* a T? that the compiler proved non-null was null */

void *sd_alloc(size_t n);             /* malloc; out of memory ends the program */
void *sd_realloc(void *p, size_t n);
void sd_free(void *p);
int64_t sd_live_objects(void);        /* only counts when built with -DSD_COUNT_OBJECTS */

/* ======================================================================
 * int: a tagged 64-bit word.
 *   w & 1 == 0 : small integer, value = w >> 1  (range -2^62 .. 2^62-1)
 *   w & 1 == 1 : pointer to sd_bigint | 1 (only for values outside that range)
 *   w == 1     : SD_INT_NULL, the null of int?
 * Values are always normalised: a value that fits is always small.
 * ====================================================================== */

struct sd_bigint {
  int64_t rc;
  int32_t sign; /* -1 or 1 */
  int32_t n;    /* number of 32-bit limbs, d[n-1] != 0 */
  uint32_t d[];
};

#define SD_INT_SMALL_MIN (-(INT64_C(1) << 62))
#define SD_INT_SMALL_MAX ((INT64_C(1) << 62) - 1)
/* A constant that fits in 62 bits plus sign (the code generator checks). */
#define SD_INT(c) ((sd_int){(int64_t)((uint64_t)(int64_t)(c) << 1)})
#define SD_INT_NULL ((sd_int){1})

SD_INLINE bool sd_int_is_small(sd_int a) { return (a.w & 1) == 0; }
SD_INLINE bool sd_int_is_null(sd_int a) { return a.w == 1; }
SD_INLINE int64_t sd_int_small_value(sd_int a) { return a.w >> 1; }

void sd_bigint_free_(sd_bigint *b);
SD_INLINE sd_bigint *sd_int_big_(sd_int a) { return (sd_bigint *)(uintptr_t)(a.w - 1); }
SD_INLINE sd_int sd_int_retain(sd_int a) {
  if (SD_UNLIKELY(a.w & 1) && a.w != 1) {
    sd_bigint *b = sd_int_big_(a);
    if (b->rc > 0) b->rc++;
  }
  return a;
}
SD_INLINE void sd_int_release(sd_int a) {
  if (SD_UNLIKELY(a.w & 1) && a.w != 1) {
    sd_bigint *b = sd_int_big_(a);
    if (b->rc > 0 && --b->rc == 0) sd_bigint_free_(b);
  }
}

sd_int sd_int_from_i64(int64_t v);           /* owned */
sd_int sd_int_from_u64(uint64_t v);          /* owned */
bool sd_int_to_i64(sd_int a, int64_t *out);  /* false if it doesn't fit */
/* A literal in source form (decimal or 0x hex, optional leading '-').
 * sd_int_from_lit returns an owned value; SD_INT_BIGLIT caches an immortal
 * value per call site (thread-safe), so its result needs no release. */
sd_int sd_int_from_lit(const char *text);
sd_int sd_int_biglit_(void *cache, const char *text);
#define SD_INT_BIGLIT(text) ({ static uintptr_t sd__cache_; sd_int_biglit_(&sd__cache_, (text)); })

sd_int sd_int_add_slow(sd_int a, sd_int b);
sd_int sd_int_sub_slow(sd_int a, sd_int b);
sd_int sd_int_mul_slow(sd_int a, sd_int b);
sd_int sd_int_div_slow(sd_int a, sd_int b);
sd_int sd_int_mod_slow(sd_int a, sd_int b);
sd_int sd_int_neg_slow(sd_int a);
sd_int sd_int_and_slow(sd_int a, sd_int b);
sd_int sd_int_or_slow(sd_int a, sd_int b);
sd_int sd_int_xor_slow(sd_int a, sd_int b);
sd_int sd_int_shl_slow(sd_int a, sd_int b);
sd_int sd_int_shr_slow(sd_int a, sd_int b);
int sd_int_cmp_slow(sd_int a, sd_int b);
bool sd_int_eq_slow(sd_int a, sd_int b);
SD_NORETURN void sd_crash_div_zero(void);

/* All int operations: arguments borrowed, result owned. */
SD_INLINE sd_int sd_int_add(sd_int a, sd_int b) {
  int64_t r;
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && SD_LIKELY(!__builtin_add_overflow(a.w, b.w, &r)))
    return (sd_int){r};
  return sd_int_add_slow(a, b);
}
SD_INLINE sd_int sd_int_sub(sd_int a, sd_int b) {
  int64_t r;
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && SD_LIKELY(!__builtin_sub_overflow(a.w, b.w, &r)))
    return (sd_int){r};
  return sd_int_sub_slow(a, b);
}
SD_INLINE sd_int sd_int_mul(sd_int a, sd_int b) {
  int64_t r;
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && SD_LIKELY(!__builtin_mul_overflow(a.w, b.w >> 1, &r)))
    return (sd_int){r};
  return sd_int_mul_slow(a, b);
}
/* Truncating division; crashes on division by zero. */
SD_INLINE sd_int sd_int_div(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && b.w != 0) {
    int64_t q = (a.w >> 1) / (b.w >> 1);
    if (SD_LIKELY(q <= SD_INT_SMALL_MAX)) return (sd_int){(int64_t)((uint64_t)q << 1)};
  }
  return sd_int_div_slow(a, b);
}
/* Remainder with the sign of a (truncating); crashes on division by zero. */
SD_INLINE sd_int sd_int_mod(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && b.w != 0) return (sd_int){a.w % b.w};
  return sd_int_mod_slow(a, b);
}
SD_INLINE sd_int sd_int_neg(sd_int a) {
  if (SD_LIKELY((a.w & 1) == 0) && a.w != INT64_MIN) return (sd_int){-a.w};
  return sd_int_neg_slow(a);
}
/* Bitwise operations use infinite two's complement (like Python). */
SD_INLINE sd_int sd_int_and(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0)) return (sd_int){a.w & b.w};
  return sd_int_and_slow(a, b);
}
SD_INLINE sd_int sd_int_or(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0)) return (sd_int){a.w | b.w};
  return sd_int_or_slow(a, b);
}
SD_INLINE sd_int sd_int_xor(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0)) return (sd_int){a.w ^ b.w};
  return sd_int_xor_slow(a, b);
}
/* Shifts crash on a negative count. */
SD_INLINE sd_int sd_int_shl(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && b.w >= 0 && b.w < 2 * 62) {
    int64_t n = b.w >> 1;
    int64_t r = (int64_t)((uint64_t)a.w << n);
    if (SD_LIKELY((r >> n) == a.w)) return (sd_int){r};
  }
  return sd_int_shl_slow(a, b);
}
SD_INLINE sd_int sd_int_shr(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0) && b.w >= 0) {
    int64_t n = b.w >> 1;
    return (sd_int){n >= 63 ? (a.w < 0 ? -2 : 0) : ((a.w >> n) & ~(int64_t)1)};
  }
  return sd_int_shr_slow(a, b);
}
SD_INLINE bool sd_int_eq(sd_int a, sd_int b) {
  if (a.w == b.w) return true;
  if (SD_LIKELY(((a.w & b.w) & 1) == 0)) return false; /* not both big: different */
  return sd_int_eq_slow(a, b);
}
SD_INLINE int sd_int_cmp(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0)) return (a.w > b.w) - (a.w < b.w);
  return sd_int_cmp_slow(a, b);
}
SD_INLINE bool sd_int_ne(sd_int a, sd_int b) { return !sd_int_eq(a, b); }
SD_INLINE bool sd_int_lt(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0)) return a.w < b.w;
  return sd_int_cmp_slow(a, b) < 0;
}
SD_INLINE bool sd_int_le(sd_int a, sd_int b) {
  if (SD_LIKELY(((a.w | b.w) & 1) == 0)) return a.w <= b.w;
  return sd_int_cmp_slow(a, b) <= 0;
}
SD_INLINE bool sd_int_gt(sd_int a, sd_int b) { return sd_int_lt(b, a); }
SD_INLINE bool sd_int_ge(sd_int a, sd_int b) { return sd_int_le(b, a); }

/* An index for a container of length len: crashes when out of range. */
SD_INLINE int64_t sd_int_index(sd_int i, int64_t len) {
  if (SD_LIKELY((i.w & 1) == 0) && (uint64_t)(i.w >> 1) < (uint64_t)len) return i.w >> 1;
  sd_crash_index(i, len);
}

double sd_int_to_float(sd_int a);            /* float(i): nearest double */
sd_int sd_float_to_int(double f);            /* int(f): truncates; crashes on nan/inf */
uint8_t sd_int_to_byte(sd_int a);            /* byte(i): crashes unless 0..255 */
SD_INLINE sd_int sd_byte_to_int(uint8_t b) { return SD_INT(b); }
uint64_t sd_int_hash(sd_int a);

/* byte operators (comparisons are plain C). Shifts by 8 or more give 0. */
SD_INLINE uint8_t sd_byte_shl(uint8_t a, uint8_t n) { return n >= 8 ? 0 : (uint8_t)(a << n); }
SD_INLINE uint8_t sd_byte_shr(uint8_t a, uint8_t n) { return n >= 8 ? 0 : (uint8_t)(a >> n); }

/* ======================================================================
 * string: pointer to an immutable, NUL-terminated, counted byte string.
 * ====================================================================== */

struct sd_string {
  int64_t rc;
  int64_t len;
  int64_t cap; /* bytes available for data (excluding the NUL); 0 for literals */
  char data[];
};

/* A string literal (inside a function body). Immortal: no retain/release
 * needed, though calling them is harmless. Uses a GNU statement expression. */
#define SD_STR_LIT(lit) ({ \
    static const struct { int64_t rc, len, cap; char data[sizeof(lit)]; } sd__lit_ = \
      {SD_RC_IMMORTAL, (int64_t)sizeof(lit) - 1, 0, lit}; \
    (sd_str)(void *)&sd__lit_; })
/* A literal at file scope: SD_STR_STATIC(name, "text"); then SD_STR_REF(name). */
#define SD_STR_STATIC(NAME, lit) \
  static const struct { int64_t rc, len, cap; char data[sizeof(lit)]; } NAME = \
    {SD_RC_IMMORTAL, (int64_t)sizeof(lit) - 1, 0, lit}
#define SD_STR_REF(NAME) ((sd_str)(void *)&(NAME))

void sd_str_free_(sd_str s);
SD_INLINE sd_str sd_str_retain(sd_str s) {
  if (s && s->rc > 0) s->rc++;
  return s;
}
SD_INLINE void sd_str_release(sd_str s) {
  if (s && s->rc > 0 && --s->rc == 0) sd_str_free_(s);
}

sd_str sd_str_new(const char *p, int64_t n);     /* owned copy of n bytes */
sd_str sd_str_from_cstr(const char *s);
sd_str sd_str_empty(void);                       /* immortal "" */
SD_INLINE const char *sd_str_cstr(sd_str s) { return s->data; } /* borrowed */
SD_INLINE int64_t sd_str_len_raw(sd_str s) { return s->len; }
SD_INLINE sd_int sd_str_len(sd_str s) { return SD_INT(s->len); }
SD_INLINE uint8_t sd_str_at(sd_str s, sd_int i) { return (uint8_t)s->data[sd_int_index(i, s->len)]; }
sd_str sd_str_concat(sd_str a, sd_str b);
sd_str sd_str_concat_n(const sd_str *parts, int n); /* a + b + c + ... in one allocation */
sd_str sd_str_append(sd_str a, sd_str b);        /* CONSUMES a; in place when a is unshared */
bool sd_str_eq(sd_str a, sd_str b);               /* null-safe */
int sd_str_cmp(sd_str a, sd_str b);               /* bytewise, -1/0/1 */
SD_INLINE bool sd_str_lt(sd_str a, sd_str b) { return sd_str_cmp(a, b) < 0; }
uint64_t sd_str_hash(sd_str s);
sd_str sd_str_slice(sd_str s, sd_int a, sd_int b);
bool sd_str_contains(sd_str s, sd_str sub);
sd_int sd_str_index(sd_str s, sd_str sub);        /* int?: SD_INT_NULL when absent */
bool sd_str_starts_with(sd_str s, sd_str p);
bool sd_str_ends_with(sd_str s, sd_str p);
sd_list *sd_str_split(sd_str s, sd_str sep);      /* []string */
sd_str sd_str_trim(sd_str s);
sd_str sd_str_replace(sd_str s, sd_str old, sd_str new_);
sd_str sd_str_to_upper(sd_str s);
sd_str sd_str_to_lower(sd_str s);
sd_err sd_str_to_int(sd_str s, sd_int *out);      /* on error *out = SD_INT_NULL */
sd_err sd_str_to_float(sd_str s, double *out);    /* on error *out = 0 */
sd_list *sd_str_bytes(sd_str s);                  /* []byte */
sd_list *sd_str_chars(sd_str s);                  /* []string, one per UTF-8 character */
sd_str sd_str_from_bytes(const sd_list *bytes);   /* string([]byte) */
/* .string() of the scalar types */
sd_str sd_int_to_str(sd_int a);
sd_str sd_float_to_str(double x);
sd_str sd_bool_to_str(bool b);
sd_str sd_byte_to_str(uint8_t b);

/* ======================================================================
 * []T: one allocation, header followed by len elements of et->size bytes.
 * ====================================================================== */

struct sd_list {
  int64_t rc;
  int64_t len;
  int64_t cap;
  const sd_type *et;
};
#define sd_list_data(l) ((void *)((sd_list *)(l) + 1))
/* Element i of list l of C type T, as an lvalue (read only: use
 * sd_list_at_mut to write). Crashes when out of range. l is evaluated
 * twice: pass a variable or a side-effect-free expression. */
#define SD_LIST_GET(l, T, i) (((T *)sd_list_data(l))[sd_int_index((i), (l)->len)])

void sd_list_free_(sd_list *l);
SD_INLINE sd_list *sd_list_retain(sd_list *l) {
  if (l && l->rc > 0) l->rc++;
  return l;
}
SD_INLINE void sd_list_release(sd_list *l) {
  if (l && l->rc > 0 && --l->rc == 0) sd_list_free_(l);
}

sd_list *sd_list_new(const sd_type *et, int64_t cap);      /* owned, empty */
SD_INLINE sd_int sd_list_len(const sd_list *l) { return SD_INT(l->len); }
void *sd_list_mut(sd_list **pl);                            /* make unique; returns data */
void *sd_list_at_mut(sd_list **pl, sd_int i);               /* unique; borrowed element pointer */
void sd_list_set(sd_list **pl, sd_int i, const void *val);  /* xs[i] = v: CONSUMES val */
void sd_list_push(sd_list **pl, const void *val);           /* CONSUMES val */
bool sd_list_pop(sd_list **pl, void *out);                  /* moves last into *out; false if empty */
void sd_list_insert(sd_list **pl, sd_int i, const void *val); /* CONSUMES val; 0 <= i <= len */
void sd_list_remove(sd_list **pl, sd_int i);
bool sd_list_contains(const sd_list *l, const void *x);
sd_int sd_list_index(const sd_list *l, const void *x);      /* int?: SD_INT_NULL when absent */
sd_list *sd_list_slice(const sd_list *l, sd_int a, sd_int b);
void sd_list_reverse(sd_list **pl);
void sd_list_sort(sd_list **pl);                            /* []int, []float, []string, []byte */
/* sortBy: stable. less(ctx, a, b) gets borrowed pointers to two elements. */
void sd_list_sort_by(sd_list **pl, bool (*less)(void *ctx, const void *a, const void *b), void *ctx);
sd_str sd_list_join(const sd_list *l, sd_str sep);          /* []string only */
bool sd_list_equal(const sd_list *a, const sd_list *b);
sd_list *sd_list_clone_deep(const sd_list *l);

/* Fast inline push for a known C element type T (the value expression v is
 * CONSUMED and evaluated exactly once; pl may be evaluated twice). */
#define SD_LIST_PUSH(pl, T, v) do { \
    sd_list *sd__l_ = *(pl); \
    if (SD_LIKELY(sd__l_->rc == 1 && sd__l_->len < sd__l_->cap)) { \
      ((T *)sd_list_data(sd__l_))[sd__l_->len++] = (v); \
    } else { \
      T sd__v_ = (v); \
      sd_list_push((pl), &sd__v_); \
    } \
  } while (0)

/* ======================================================================
 * map[K]V: insertion-ordered hash map. K is int, byte, bool or string.
 * Keys and values are passed by pointer to their C representation.
 * ====================================================================== */

void sd_map_free_(sd_map *m);
SD_INLINE sd_map *sd_map_retain(sd_map *m);
SD_INLINE void sd_map_release(sd_map *m);

sd_map *sd_map_new(const sd_type *kt, const sd_type *vt);  /* owned, empty */
sd_int sd_map_len(const sd_map *m);
void *sd_map_get(const sd_map *m, const void *key);         /* borrowed value pointer or NULL */
void *sd_map_get_mut(sd_map **pm, const void *key);         /* unique; borrowed or NULL */
void sd_map_set(sd_map **pm, const void *key, const void *val); /* CONSUMES key and val */
void sd_map_delete(sd_map **pm, const void *key);
sd_list *sd_map_keys(const sd_map *m);                      /* []K in insertion order */
/* Iterate in insertion order: int64_t pos = 0; void *k, *v;
 * while (sd_map_next(m, &pos, &k, &v)) { ... } (borrowed pointers) */
bool sd_map_next(const sd_map *m, int64_t *pos, void **key, void **val);
bool sd_map_equal(const sd_map *a, const sd_map *b);        /* same keys and values, any order */
sd_map *sd_map_clone_deep(const sd_map *m);

/* ======================================================================
 * Boxes: a counted heap cell holding one value of type t. Used for
 * nullable structs (S? = sd_box*, NULL = null), recursive types, enums and
 * interface payloads. Copy-on-write like lists.
 * ====================================================================== */

struct sd_box {
  int64_t rc;
  const sd_type *t;
};
#define sd_box_data(b) ((void *)((sd_box *)(b) + 1))
#define SD_BOX_GET(b, T) (*(T *)sd_box_data(b))
/* An immortal box at file scope, e.g. for enum cases without data:
 * SD_BOX_STATIC(name, &T_Token, E_Token, {.tag = 3}); then SD_BOX_REF(name). */
#define SD_BOX_STATIC(NAME, TYPE, CTYPE, ...) \
  static struct { sd_box h; CTYPE v; } NAME = {{SD_RC_IMMORTAL, TYPE}, __VA_ARGS__}
#define SD_BOX_REF(NAME) (&(NAME).h)

void sd_box_free_(sd_box *b);
SD_INLINE sd_box *sd_box_retain(sd_box *b) {
  if (b && b->rc > 0) b->rc++;
  return b;
}
SD_INLINE void sd_box_release(sd_box *b) {
  if (b && b->rc > 0 && --b->rc == 0) sd_box_free_(b);
}
sd_box *sd_box_new(const sd_type *t, const void *val);  /* CONSUMES val */
void *sd_box_mut(sd_box **pb);                           /* make unique; returns data */
bool sd_box_equal(const sd_box *a, const sd_box *b);     /* null-safe */
sd_box *sd_box_clone_deep(const sd_box *b);

/* ======================================================================
 * Interfaces and errors
 * ====================================================================== */

#define SD_IFACE_NULL ((sd_iface){NULL, NULL})
#define SD_ERR_NULL ((sd_err){NULL, NULL})
SD_INLINE bool sd_iface_is_null(sd_iface v) { return v.vt == NULL; }
SD_INLINE bool sd_err_is_null(sd_err e) { return e.vt == NULL; }
/* Wrap a value of type t (CONSUMED) with vtable vt. */
sd_iface sd_iface_new(const sd_vtable *vt, const sd_type *t, const void *val);
SD_INLINE sd_iface sd_iface_retain(sd_iface v) {
  sd_box_retain(v.obj);
  return v;
}
SD_INLINE void sd_iface_release(sd_iface v) { sd_box_release(v.obj); }
SD_INLINE const void *sd_iface_self(sd_iface v) { return sd_box_data(v.obj); }
bool sd_iface_equal(sd_iface a, sd_iface b);
#define sd_err_retain sd_iface_retain
#define sd_err_release sd_iface_release

sd_err sd_errors_new(sd_str msg);                 /* errors.new: msg borrowed */
sd_err sd_errorf(const char *fmt, ...) SD_PRINTF(1, 2);
sd_str sd_err_message(sd_err e);                  /* e.message(); crashes on null */
bool sd_err_equal(sd_err a, sd_err b);            /* both null, or same message */
extern const sd_error_vtable sd_errors_vtable;   /* the vtable of errors.new values */

/* ======================================================================
 * Goroutines, defer, groups, channels, select
 * ====================================================================== */

/* ---- defer ----
 *   size_t mark = sd_defer_mark();               at function entry
 *   sd_defer_push(thunk, &env, sizeof env);      at each defer statement
 *   sd_defer_run(mark);                          before every return
 * The env bytes are copied; the thunk receives a pointer to a copy and must
 * release whatever the env owns. On a crash or cancellation the runtime runs
 * all pending thunks of the goroutine, newest first. */
size_t sd_defer_mark(void);
void sd_defer_push(void (*thunk)(void *env), const void *env, size_t size);
void sd_defer_run(size_t mark);

/* ---- groups and spawn ----
 *   sd_group *g = sd_group_begin();
 *   sd_spawn(g, body, &T_args, &args);  (args borrowed: deep-copied for the goroutine)
 *   sd_err err = sd_group_wait(g);       (owned; g is freed)
 * body(args) runs on its own thread with a borrowed pointer to its private
 * copy of the arguments, and returns an owned error. */
typedef struct sd_group sd_group;
typedef sd_err (*sd_task_fn)(void *args);
sd_group *sd_group_begin(void);
void sd_spawn(sd_group *g, sd_task_fn body, const sd_type *args_type, const void *args);
sd_err sd_group_wait(sd_group *g);

/* Runs fn on the current goroutine's thread when the goroutine ends (newest
 * first); never for the main goroutine. Generated code uses it to release
 * the constants a goroutine computed for itself (_Thread_local caches). */
void sd_goroutine_atexit(void (*fn)(void));

/* ---- channels ---- (channel handles are shared between goroutines; their
 * count is atomic) */
sd_chan *sd_chan_new(const sd_type *et, sd_int cap); /* chan T(n); crashes if n < 0 */
sd_chan *sd_chan_retain(sd_chan *c);
void sd_chan_release(sd_chan *c);
void sd_chan_send(sd_chan *c, const void *val);       /* val borrowed; the receiver gets a deep copy */
bool sd_chan_receive(sd_chan *c, void *out);          /* false: closed and empty (out untouched) */
void sd_chan_close(sd_chan *c);                       /* crashes if already closed */

enum { SD_SEND = 1, SD_RECV = 2 };
typedef struct sd_select_case {
  sd_chan *ch;
  int dir;     /* SD_SEND or SD_RECV */
  void *val;   /* SD_SEND: borrowed value to send; SD_RECV: where to store the received value */
  bool ok;     /* set for the chosen SD_RECV case: true = value stored, false = closed */
} sd_select_case;
/* Waits until one case can proceed, performs it and returns its index. When
 * several are ready, one is chosen uniformly at random. */
int sd_select(sd_select_case *cases, int n);

/* ---- time ---- */
sd_chan *sd_time_after(sd_int ns);  /* chan int receiving the wall-clock time once, after ns, then closed */
void sd_time_sleep(sd_int ns);      /* a cancellation point, like a receive */
sd_int sd_time_now(void);           /* monotonic nanoseconds */

/* ---- program entry and exit ----
 *   int main(int argc, char **argv) { return sd_main(argc, argv, sd_f_main); }
 * Runs the Seed main with an owned []string of the arguments. Returning an
 * error prints "error: <message>" and exits 1; a crash prints
 * "crash: <message>" and exits 2. Stdout is flushed before any exit. */
int sd_main(int argc, char **argv, sd_err (*main_fn)(sd_list *args));
/* Run fn(arg) in a protected frame: a crash inside it runs the defers it
 * pushed and comes back as an owned error "crash: <message>" (for tests). */
sd_err sd_catch(sd_err (*fn)(void *arg), void *arg);
SD_NORETURN void sd_exit_process(int code); /* flush stdout/stderr and _exit */

/* ======================================================================
 * Core packages
 * ====================================================================== */

/* fmt */
void sd_fmt_print(sd_str s);
void sd_fmt_println(sd_str s);
void sd_fmt_eprint(sd_str s);
void sd_fmt_eprintln(sd_str s);
/* os */
SD_NORETURN void sd_os_exit(sd_int code);
sd_str sd_os_env(sd_str name);                      /* string?: NULL when unset */
sd_err sd_os_run(const sd_list *argv, sd_int *code); /* on error *code = SD_INT_NULL */
/* fs (on error the out value is NULL) */
sd_err sd_fs_read(sd_str path, sd_str *out);
sd_err sd_fs_write(sd_str path, sd_str data);
bool sd_fs_exists(sd_str path);
bool sd_fs_is_dir(sd_str path);
sd_err sd_fs_list(sd_str dir, sd_list **out);
sd_err sd_fs_mkdir(sd_str path);
sd_err sd_fs_remove(sd_str path);

/* ======================================================================
 * Inline definitions that need the full struct layouts.
 * ====================================================================== */

struct sd_map {
  int64_t rc;
  int64_t len;      /* live entries */
  int64_t n;        /* used entry slots (live + deleted) */
  int64_t cap;      /* entry slots allocated */
  int64_t icap;     /* index slots (power of two), 0 when empty */
  const sd_type *kt, *vt;
  int32_t koff, voff, esz; /* entry layout: [hash u64][live u64][key][value] */
  char *entries;
  int32_t *index;   /* 0 empty, -1 deleted, k+1 entry k */
};

SD_INLINE sd_map *sd_map_retain(sd_map *m) {
  if (m && m->rc > 0) m->rc++;
  return m;
}
SD_INLINE void sd_map_release(sd_map *m) {
  if (m && m->rc > 0 && --m->rc == 0) sd_map_free_(m);
}

#endif /* SEED_H */
