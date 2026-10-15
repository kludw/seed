/* Natives of package regex (runtime/README.md, "Native functions"):
 * regular expressions with Go's regexp syntax and semantics (RE2: no
 * backreferences, no lookaround), matched in time linear in the input.
 *
 * How it works (lib/README.md, "regex", has more):
 *
 * - The pattern is parsed into a tree (rx_node), with case folding,
 *   classes and Unicode tables resolved into sorted rune ranges.
 * - The tree is compiled twice into byte-level NFA programs (rx_prog):
 *   forward, with capture slots, and reversed, without them. Rune ranges
 *   become UTF-8 byte sequences, so the machines read bytes. Counted
 *   repetition is expanded the way Go's regexp/syntax simplifies it, and
 *   the program has the same shape and priorities as Go's, so that
 *   leftmost-first matching chooses the same match and the same groups.
 * - Searching runs a lazily built DFA over the forward program (the
 *   states are ordered thread lists, so it keeps Pike VM priorities and
 *   finds where the leftmost-first match ends), then a DFA over the
 *   reversed program backwards from that end (longest match: the leftmost
 *   start), then, only if the pattern has groups, a Pike VM anchored at
 *   that start for the group positions. Every step is linear: a DFA state
 *   is built in time proportional to the program, at most one per byte,
 *   and the state cache is flushed when it outgrows its budget.
 * - Empty-width assertions (^ $ \A \z \b \B) are decided at each boundary
 *   from the bytes on both sides, which a DFA state remembers for the
 *   side it has read. New match attempts start only at UTF-8 character
 *   boundaries, as in Go, which steps through the text by runes.
 * - Compiled regexes are cached per goroutine (keyed by the id the Seed
 *   value carries), so the Seed value stays plain data (a pattern and an
 *   id) that can be copied and sent to other goroutines; a goroutine that
 *   meets an id it has not seen compiles the pattern again.
 *
 * Nothing here parks, so goroutine-local storage is safe to keep across
 * the calls of one native (runtime/README.md, "Thread-local storage"). */
#include "seed.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* regex_tables.c (generated) */
static int32_t rx_simple_fold(int32_t c);
static const int32_t *rx_unicode_table(const char *name, size_t len, int *n);
static int rx_unicode_category_count(void);
static const char *rx_unicode_category_name(int i);

#define RX_MAX_RUNE 0x10FFFF
#define RX_MAX_REPEAT 1000
#define RX_MAX_DEPTH 1000
#define RX_MAX_INST 400000
#define RX_DFA_BUDGET ((size_t)2 << 20)
#define RX_CACHE_N 16

/* ------------------------------------------------------------- arena */

typedef struct rx_block {
  struct rx_block *next;
  size_t used, cap;
  uint64_t data[];
} rx_block;

typedef struct {
  rx_block *head;
} rx_arena;

static void *rx_alloc(rx_arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (!a->head || a->head->used + n > a->head->cap) {
    size_t cap = n > 65536 ? n : 65536;
    rx_block *b = sd_alloc(sizeof(rx_block) + cap);
    b->next = a->head;
    b->used = 0;
    b->cap = cap;
    a->head = b;
  }
  void *p = (char *)a->head->data + a->head->used;
  a->head->used += n;
  return p;
}

/* Zeroed memory; out of memory ends the program, as sd_alloc does. */
static void *rx_zalloc(size_t n) {
  void *p = sd_alloc(n ? n : 1);
  memset(p, 0, n ? n : 1);
  return p;
}

static void rx_arena_free(rx_arena *a) {
  while (a->head) {
    rx_block *b = a->head;
    a->head = b->next;
    free(b);
  }
}

/* ------------------------------------------------- bytes and assertions */

/* What is on one side of a position: the edge of the text, a newline, an
 * ASCII word character or another byte. */
enum { RX_EDGE = 0, RX_NL = 1, RX_WORD = 2, RX_OTHER = 3 };

static bool rx_is_word(int c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int rx_side(int c) {
  if (c < 0) return RX_EDGE;
  if (c == '\n') return RX_NL;
  return rx_is_word(c) ? RX_WORD : RX_OTHER;
}

enum { RX_BOT, RX_EOT, RX_BOL, RX_EOL, RX_WB, RX_NWB };

static bool rx_assert_ok(int kind, int left, int right) {
  switch (kind) {
  case RX_BOT: return left == RX_EDGE;
  case RX_EOT: return right == RX_EDGE;
  case RX_BOL: return left == RX_EDGE || left == RX_NL;
  case RX_EOL: return right == RX_EDGE || right == RX_NL;
  case RX_WB: return (left == RX_WORD) != (right == RX_WORD);
  default: return (left == RX_WORD) == (right == RX_WORD);
  }
}

/* UTF-8 character boundaries, decided the way Go decodes (an invalid byte
 * is a character of its own). dec is what the bytes so far still need:
 * 0 nothing; 1 one continuation; 2 two; 3 two, the first A0-BF (after
 * E0); 4 two, the first 80-9F (after ED); 5 three; 6 three, the first
 * 90-BF (after F0); 7 three, the first 80-8F (after F4). */
static bool rx_cont_ok(int dec, int c) {
  switch (dec) {
  case 0: return false;
  case 3: return c >= 0xA0 && c <= 0xBF;
  case 4: return c >= 0x80 && c <= 0x9F;
  case 6: return c >= 0x90 && c <= 0xBF;
  case 7: return c >= 0x80 && c <= 0x8F;
  default: return c >= 0x80 && c <= 0xBF;
  }
}

/* Whether the position before byte c is a character boundary. */
static bool rx_boundary(int dec, int c) { return dec == 0 || !rx_cont_ok(dec, c); }

static int rx_dec_after(int dec, int c, bool boundary) {
  if (!boundary) return dec == 1 ? 0 : dec <= 4 ? 1 : 2;
  if (c < 0x80) return 0;
  if (c >= 0xC2 && c <= 0xDF) return 1;
  if (c == 0xE0) return 3;
  if (c == 0xED) return 4;
  if (c >= 0xE1 && c <= 0xEF) return 2;
  if (c == 0xF0) return 6;
  if (c == 0xF4) return 7;
  if (c >= 0xF1 && c <= 0xF3) return 5;
  return 0;
}

/* How many continuation bytes dec still needs. */
static int rx_need(int dec) { return dec == 0 ? 0 : dec == 1 ? 1 : dec <= 4 ? 2 : 3; }

/* The width of the character at t[i] as Go's utf8.DecodeRune counts it
 * (1 for an invalid byte). */
static int rx_width(const uint8_t *t, int64_t n, int64_t i) {
  int dec = rx_dec_after(0, t[i], true);
  int need = rx_need(dec);
  if (i + need >= n) return 1;
  for (int k = 1; k <= need; k++) {
    if (!rx_cont_ok(dec, t[i + k])) return 1;
    dec = rx_dec_after(dec, t[i + k], false);
  }
  return need + 1;
}

/* Whether position j is a character boundary: not inside a valid UTF-8
 * sequence that starts before it. */
static bool rx_at_boundary(const uint8_t *t, int64_t n, int64_t j) {
  if (j <= 0 || j >= n || t[j] < 0x80 || t[j] > 0xBF) return true;
  for (int k = 1; k <= 3 && j - k >= 0; k++) {
    int c = t[j - k];
    if (c < 0x80 || c > 0xBF) return !(c >= 0xC2 && c <= 0xF4 && rx_width(t, n, j - k) > k);
  }
  return true;
}

/* --------------------------------------------------------- the tree */

enum { RX_FOLD = 1, RX_MULTI = 2, RX_DOTNL = 4, RX_UNGREEDY = 8 };
enum { N_EMPTY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_CAP, N_ASSERT };

typedef struct rx_node rx_node;
struct rx_node {
  uint8_t kind;
  bool greedy;
  uint8_t akind; /* N_ASSERT */
  int n;         /* N_CAT, N_ALT: subs; N_CLASS: ranges */
  int min, max;  /* N_REPEAT; max -1: unbounded */
  int cap;       /* N_CAP */
  rx_node **subs;
  int32_t *pairs; /* N_CLASS: n ranges lo, hi, sorted and disjoint */
};

/* A class being built: n ranges in p[0 .. 2n). */
typedef struct {
  int32_t *p;
  int n, cap;
} rx_cls;

typedef struct {
  const uint8_t *s;
  size_t n, i;
  rx_arena ar;
  int ncap;
  const uint8_t **name; /* name[k], namelen[k]: group k's name (NULL: none) */
  size_t *namelen;
  int namecap;
  int depth;
  jmp_buf fail;
  char msg[300];
} rx_parser;

static SD_NORETURN void rx_error(rx_parser *ps, size_t off, const char *fmt, ...) {
  char reason[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(reason, sizeof reason, fmt, ap);
  va_end(ap);
  snprintf(ps->msg, sizeof ps->msg, "%s at offset %zu", reason, off);
  longjmp(ps->fail, 1);
}

static rx_node *rx_new(rx_parser *ps, int kind) {
  rx_node *x = rx_alloc(&ps->ar, sizeof *x);
  memset(x, 0, sizeof *x);
  x->kind = (uint8_t)kind;
  return x;
}

/* ----------------------------------------------------------- classes */

/* Adds a range, merging it into the last one when they touch (Go's
 * appendRange); rx_cls_norm sorts and merges the rest. */
static void rx_cls_push(rx_parser *ps, rx_cls *c, int32_t lo, int32_t hi) {
  for (int k = c->n - 1; k >= 0 && k >= c->n - 2; k--) {
    int32_t *r = &c->p[2 * k];
    if (lo <= r[1] + 1 && r[0] <= hi + 1) {
      if (lo < r[0]) r[0] = lo;
      if (hi > r[1]) r[1] = hi;
      return;
    }
  }
  if (c->n == c->cap) {
    int cap = c->cap ? c->cap * 2 : 8;
    int32_t *p = rx_alloc(&ps->ar, (size_t)cap * 2 * sizeof(int32_t));
    if (c->n) memcpy(p, c->p, (size_t)c->n * 2 * sizeof(int32_t));
    c->p = p;
    c->cap = cap;
  }
  c->p[2 * c->n] = lo;
  c->p[2 * c->n + 1] = hi;
  c->n++;
}

static int rx_range_cmp(const void *a, const void *b) {
  const int32_t *x = a, *y = b;
  return x[0] < y[0] ? -1 : x[0] > y[0] ? 1 : 0;
}

static void rx_cls_norm(rx_cls *c) {
  if (c->n < 2) return;
  qsort(c->p, (size_t)c->n, 2 * sizeof(int32_t), rx_range_cmp);
  int w = 0;
  for (int k = 1; k < c->n; k++) {
    int32_t *last = &c->p[2 * w], *r = &c->p[2 * k];
    if (r[0] <= last[1] + 1) {
      if (r[1] > last[1]) last[1] = r[1];
    } else {
      w++;
      c->p[2 * w] = r[0];
      c->p[2 * w + 1] = r[1];
    }
  }
  c->n = w + 1;
}

/* The complement of a normalised class. */
static void rx_cls_negate(rx_parser *ps, rx_cls *c) {
  rx_cls out = {0};
  int32_t next = 0;
  for (int k = 0; k < c->n; k++) {
    if (c->p[2 * k] > next) rx_cls_push(ps, &out, next, c->p[2 * k] - 1);
    next = c->p[2 * k + 1] + 1;
  }
  if (next <= RX_MAX_RUNE) rx_cls_push(ps, &out, next, RX_MAX_RUNE);
  *c = out;
}

/* Adds lo-hi and, with fold, every rune of their case-folding orbits
 * (Go's appendFoldedRange). */
#define RX_MIN_FOLD 0x0041
#define RX_MAX_FOLD 0x1E943
static void rx_cls_add(rx_parser *ps, rx_cls *c, int32_t lo, int32_t hi, bool fold) {
  if (!fold || (lo <= RX_MIN_FOLD && hi >= RX_MAX_FOLD) || hi < RX_MIN_FOLD || lo > RX_MAX_FOLD) {
    rx_cls_push(ps, c, lo, hi);
    return;
  }
  if (lo < RX_MIN_FOLD) {
    rx_cls_push(ps, c, lo, RX_MIN_FOLD - 1);
    lo = RX_MIN_FOLD;
  }
  if (hi > RX_MAX_FOLD) {
    rx_cls_push(ps, c, RX_MAX_FOLD + 1, hi);
    hi = RX_MAX_FOLD;
  }
  for (int32_t r = lo; r <= hi; r++) {
    rx_cls_push(ps, c, r, r);
    for (int32_t f = rx_simple_fold(r); f != r; f = rx_simple_fold(f)) rx_cls_push(ps, c, f, f);
  }
}

/* Adds the ranges pairs[0 .. 2n), folded with fold, negated with neg
 * (Go's appendGroup: fold first, then negate). */
static void rx_cls_add_group(rx_parser *ps, rx_cls *dst, const int32_t *pairs, int n, bool neg, bool fold) {
  rx_cls tmp = {0};
  for (int k = 0; k < n; k++) rx_cls_add(ps, &tmp, pairs[2 * k], pairs[2 * k + 1], fold);
  rx_cls_norm(&tmp);
  if (neg) rx_cls_negate(ps, &tmp);
  for (int k = 0; k < tmp.n; k++) rx_cls_push(ps, dst, tmp.p[2 * k], tmp.p[2 * k + 1]);
}

static rx_node *rx_class_node(rx_parser *ps, rx_cls *c) {
  rx_cls_norm(c);
  rx_node *x = rx_new(ps, N_CLASS);
  x->n = c->n;
  x->pairs = c->p;
  return x;
}

static const int32_t rx_perl_d[] = {'0', '9'};
static const int32_t rx_perl_s[] = {'\t', '\n', '\f', '\r', ' ', ' '};
static const int32_t rx_perl_w[] = {'0', '9', 'A', 'Z', '_', '_', 'a', 'z'};

/* \d \s \w and their negations; c is the letter. */
static void rx_perl_class(rx_parser *ps, rx_cls *dst, int c, bool fold) {
  bool neg = c == 'D' || c == 'S' || c == 'W';
  switch (c | 0x20) {
  case 'd': rx_cls_add_group(ps, dst, rx_perl_d, 1, neg, fold); break;
  case 's': rx_cls_add_group(ps, dst, rx_perl_s, 3, neg, fold); break;
  default: rx_cls_add_group(ps, dst, rx_perl_w, 4, neg, fold); break;
  }
}

static const struct {
  const char *name;
  int32_t pairs[8];
  int n;
} rx_posix[] = {
    {"alnum", {'0', '9', 'A', 'Z', 'a', 'z'}, 3},
    {"alpha", {'A', 'Z', 'a', 'z'}, 2},
    {"ascii", {0, 0x7F}, 1},
    {"blank", {'\t', '\t', ' ', ' '}, 2},
    {"cntrl", {0, 0x1F, 0x7F, 0x7F}, 2},
    {"digit", {'0', '9'}, 1},
    {"graph", {'!', '~'}, 1},
    {"lower", {'a', 'z'}, 1},
    {"print", {' ', '~'}, 1},
    {"punct", {'!', '/', ':', '@', '[', '`', '{', '~'}, 4},
    {"space", {'\t', '\r', ' ', ' '}, 2},
    {"upper", {'A', 'Z'}, 1},
    {"word", {'0', '9', 'A', 'Z', '_', '_', 'a', 'z'}, 4},
    {"xdigit", {'0', '9', 'A', 'F', 'a', 'f'}, 3},
};

/* \pX, \p{Name}, \p{^Name}, \PX, \P{Name}: the cursor is on the p or P. */
static void rx_unicode_class(rx_parser *ps, rx_cls *dst, bool fold) {
  size_t start = ps->i - 1;
  bool neg = ps->s[ps->i] == 'P';
  ps->i++;
  if (ps->i >= ps->n) rx_error(ps, start, "invalid Unicode class escape");
  const uint8_t *name;
  size_t len;
  if (ps->s[ps->i] == '{') {
    size_t j = ps->i + 1;
    while (j < ps->n && ps->s[j] != '}') j++;
    if (j >= ps->n) rx_error(ps, start, "missing closing '}' in Unicode class");
    name = ps->s + ps->i + 1;
    len = j - ps->i - 1;
    ps->i = j + 1;
    if (len > 0 && name[0] == '^') {
      neg = !neg;
      name++;
      len--;
    }
  } else {
    name = ps->s + ps->i;
    len = 1;
    ps->i++;
  }
  rx_cls tmp = {0};
  if (len == 3 && memcmp(name, "Any", 3) == 0) {
    rx_cls_push(ps, &tmp, 0, RX_MAX_RUNE);
  } else if (len == 1) {
    /* A one-letter category is the union of its two-letter ones. */
    bool found = false;
    for (int i = 0; i < rx_unicode_category_count(); i++) {
      const char *cat = rx_unicode_category_name(i);
      if (cat[0] != (char)name[0]) continue;
      int n;
      const int32_t *r = rx_unicode_table(cat, strlen(cat), &n);
      for (int k = 0; k < n; k++) rx_cls_add(ps, &tmp, r[2 * k], r[2 * k + 1], fold);
      found = true;
    }
    if (!found) rx_error(ps, start, "unknown Unicode class %.*s", (int)len, (const char *)name);
  } else {
    int n;
    const int32_t *r = rx_unicode_table((const char *)name, len, &n);
    if (!r) rx_error(ps, start, "unknown Unicode class %.*s", (int)len, (const char *)name);
    for (int k = 0; k < n; k++) rx_cls_add(ps, &tmp, r[2 * k], r[2 * k + 1], fold);
  }
  rx_cls_norm(&tmp);
  if (neg) rx_cls_negate(ps, &tmp);
  for (int k = 0; k < tmp.n; k++) rx_cls_push(ps, dst, tmp.p[2 * k], tmp.p[2 * k + 1]);
}

/* ------------------------------------------------------------ parsing */

/* Decodes the UTF-8 character at the cursor and moves past it. */
static int32_t rx_next_rune(rx_parser *ps) {
  const uint8_t *s = ps->s;
  size_t i = ps->i;
  int c = s[i];
  if (c < 0x80) {
    ps->i++;
    return c;
  }
  int dec = rx_dec_after(0, c, true);
  int need = rx_need(dec);
  if (need == 0 || i + (size_t)need >= ps->n) rx_error(ps, i, "invalid UTF-8");
  int32_t r = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
  for (int k = 1; k <= need; k++) {
    if (!rx_cont_ok(dec, s[i + k])) rx_error(ps, i, "invalid UTF-8");
    dec = rx_dec_after(dec, s[i + k], false);
    r = (r << 6) | (s[i + k] & 0x3F);
  }
  ps->i = i + (size_t)need + 1;
  return r;
}

static int rx_hex(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool rx_is_alnum(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* An escape that stands for one character; the cursor is after the
 * backslash, start is the backslash. */
static int32_t rx_escape_rune(rx_parser *ps, size_t start) {
  if (ps->i >= ps->n) rx_error(ps, start, "trailing backslash");
  int c = ps->s[ps->i];
  if (c >= '1' && c <= '7' && (ps->i + 1 >= ps->n || ps->s[ps->i + 1] < '0' || ps->s[ps->i + 1] > '7'))
    rx_error(ps, start, "backreferences are not supported: \\%c", c);
  if (c >= '0' && c <= '7') {
    /* Octal: up to three digits. */
    int32_t r = 0;
    for (int k = 0; k < 3 && ps->i < ps->n && ps->s[ps->i] >= '0' && ps->s[ps->i] <= '7'; k++) r = r * 8 + (ps->s[ps->i++] - '0');
    return r;
  }
  ps->i++;
  switch (c) {
  case 'a': return 7;
  case 'f': return '\f';
  case 'n': return '\n';
  case 'r': return '\r';
  case 't': return '\t';
  case 'v': return '\v';
  case 'x': {
    if (ps->i < ps->n && ps->s[ps->i] == '{') {
      ps->i++;
      int32_t r = 0;
      int digits = 0;
      while (ps->i < ps->n && ps->s[ps->i] != '}') {
        int h = rx_hex(ps->s[ps->i]);
        if (h < 0) rx_error(ps, start, "invalid escape sequence");
        r = r * 16 + h;
        if (r > RX_MAX_RUNE) rx_error(ps, start, "invalid escape sequence: character code above \\x{10FFFF}");
        digits++;
        ps->i++;
      }
      if (ps->i >= ps->n || digits == 0) rx_error(ps, start, "invalid escape sequence");
      ps->i++;
      return r;
    }
    if (ps->i + 2 > ps->n) rx_error(ps, start, "invalid escape sequence");
    int h1 = rx_hex(ps->s[ps->i]), h2 = rx_hex(ps->s[ps->i + 1]);
    if (h1 < 0 || h2 < 0) rx_error(ps, start, "invalid escape sequence");
    ps->i += 2;
    return h1 * 16 + h2;
  }
  default:
    if (c < 0x80 && !rx_is_alnum(c)) return c;
    if (c < 0x80) rx_error(ps, start, "invalid escape sequence \\%c", c);
    rx_error(ps, start, "invalid escape sequence");
  }
}

/* A bracketed class; the cursor is on the '['. */
static rx_node *rx_parse_class(rx_parser *ps, int fl) {
  size_t start = ps->i;
  bool fold = (fl & RX_FOLD) != 0;
  ps->i++;
  bool neg = false;
  if (ps->i < ps->n && ps->s[ps->i] == '^') {
    neg = true;
    ps->i++;
  }
  rx_cls c = {0};
  bool first = true;
  for (;;) {
    if (ps->i >= ps->n) rx_error(ps, start, "missing closing ']'");
    int ch = ps->s[ps->i];
    if (ch == ']' && !first) {
      ps->i++;
      break;
    }
    first = false;
    size_t itemstart = ps->i;
    /* [:alpha:] */
    if (ch == '[' && ps->i + 1 < ps->n && ps->s[ps->i + 1] == ':') {
      size_t j = ps->i + 2;
      while (j + 1 < ps->n && !(ps->s[j] == ':' && ps->s[j + 1] == ']')) j++;
      if (j + 1 < ps->n) {
        const uint8_t *name = ps->s + ps->i + 2;
        size_t len = j - ps->i - 2;
        bool pneg = len > 0 && name[0] == '^';
        if (pneg) {
          name++;
          len--;
        }
        bool found = false;
        for (size_t k = 0; k < sizeof rx_posix / sizeof rx_posix[0]; k++) {
          if (strlen(rx_posix[k].name) == len && memcmp(rx_posix[k].name, name, len) == 0) {
            rx_cls_add_group(ps, &c, rx_posix[k].pairs, rx_posix[k].n, pneg, fold);
            found = true;
            break;
          }
        }
        if (!found) rx_error(ps, itemstart, "unknown POSIX class [:%.*s:]", (int)len, (const char *)name);
        ps->i = j + 2;
        continue;
      }
    }
    int32_t lo;
    if (ch == '\\') {
      ps->i++;
      if (ps->i < ps->n) {
        int e = ps->s[ps->i];
        if (e == 'd' || e == 'D' || e == 's' || e == 'S' || e == 'w' || e == 'W') {
          ps->i++;
          rx_perl_class(ps, &c, e, fold);
          continue;
        }
        if (e == 'p' || e == 'P') {
          rx_unicode_class(ps, &c, fold);
          continue;
        }
      }
      lo = rx_escape_rune(ps, itemstart);
    } else {
      lo = rx_next_rune(ps);
    }
    int32_t hi = lo;
    if (ps->i + 1 < ps->n && ps->s[ps->i] == '-' && ps->s[ps->i + 1] != ']') {
      ps->i++;
      size_t histart = ps->i;
      if (ps->s[ps->i] == '\\') {
        ps->i++;
        if (ps->i < ps->n && ps->s[ps->i] != 0 && strchr("dDsSwWpP", ps->s[ps->i]))
          rx_error(ps, itemstart, "invalid character class range");
        hi = rx_escape_rune(ps, histart);
      } else {
        hi = rx_next_rune(ps);
      }
      if (hi < lo) rx_error(ps, itemstart, "invalid character class range");
    }
    rx_cls_add(ps, &c, lo, hi, fold);
  }
  rx_cls_norm(&c);
  if (neg) rx_cls_negate(ps, &c);
  return rx_class_node(ps, &c);
}

static rx_node *rx_literal(rx_parser *ps, int32_t r, int fl) {
  rx_cls c = {0};
  rx_cls_add(ps, &c, r, r, (fl & RX_FOLD) != 0);
  return rx_class_node(ps, &c);
}

static rx_node *rx_assert(rx_parser *ps, int kind) {
  rx_node *x = rx_new(ps, N_ASSERT);
  x->akind = (uint8_t)kind;
  return x;
}

/* A growing list of nodes. */
typedef struct {
  rx_node **v;
  int n, cap;
} rx_nodes;

static void rx_nodes_push(rx_parser *ps, rx_nodes *l, rx_node *x) {
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 8;
    rx_node **v = rx_alloc(&ps->ar, (size_t)cap * sizeof *v);
    if (l->n) memcpy(v, l->v, (size_t)l->n * sizeof *v);
    l->v = v;
    l->cap = cap;
  }
  l->v[l->n++] = x;
}

static rx_node *rx_join(rx_parser *ps, int kind, rx_nodes *l) {
  if (l->n == 0) return rx_new(ps, N_EMPTY);
  if (l->n == 1) return l->v[0];
  rx_node *x = rx_new(ps, kind);
  x->n = l->n;
  x->subs = l->v;
  return x;
}

/* {n}, {n,} or {n,m} at the cursor, as Go reads them: a '{' that does not
 * start one is a literal. Numbers have no leading zeros. */
static bool rx_parse_int(const uint8_t *s, size_t n, size_t *i, int *out) {
  size_t j = *i;
  if (j >= n || s[j] < '0' || s[j] > '9') return false;
  if (j + 1 < n && s[j] == '0' && s[j + 1] >= '0' && s[j + 1] <= '9') return false;
  int v = 0;
  for (; j < n && s[j] >= '0' && s[j] <= '9'; j++) {
    if (v >= 100000000) v = -1;
    if (v >= 0) v = v * 10 + (s[j] - '0');
  }
  *i = j;
  *out = v;
  return true;
}

static bool rx_is_repeat(rx_parser *ps, int *min, int *max, size_t *end) {
  size_t j = ps->i + 1;
  if (!rx_parse_int(ps->s, ps->n, &j, min)) return false;
  if (j >= ps->n) return false;
  if (ps->s[j] == ',') {
    j++;
    if (j >= ps->n) return false;
    if (ps->s[j] == '}') {
      *max = -1;
    } else if (!rx_parse_int(ps->s, ps->n, &j, max)) {
      return false;
    }
  } else {
    *max = *min;
  }
  if (j >= ps->n || ps->s[j] != '}') return false;
  *end = j + 1;
  return true;
}

static void rx_add_name(rx_parser *ps, int cap, const uint8_t *name, size_t len) {
  while (cap >= ps->namecap) {
    int nc = ps->namecap ? ps->namecap * 2 : 16;
    const uint8_t **nn = rx_alloc(&ps->ar, (size_t)nc * sizeof *nn);
    size_t *nl = rx_alloc(&ps->ar, (size_t)nc * sizeof *nl);
    memset(nn, 0, (size_t)nc * sizeof *nn);
    memset(nl, 0, (size_t)nc * sizeof *nl);
    if (ps->namecap) {
      memcpy(nn, ps->name, (size_t)ps->namecap * sizeof *nn);
      memcpy(nl, ps->namelen, (size_t)ps->namecap * sizeof *nl);
    }
    ps->name = nn;
    ps->namelen = nl;
    ps->namecap = nc;
  }
  ps->name[cap] = name;
  ps->namelen[cap] = len;
}

static rx_node *rx_parse_alts(rx_parser *ps, int fl, long open);

/* A group; the cursor is on the '('. Returns NULL for a flags-only group
 * (?i), after changing *fl. */
static rx_node *rx_parse_group(rx_parser *ps, int *fl) {
  size_t start = ps->i;
  const uint8_t *s = ps->s;
  ps->i++;
  if (ps->i < ps->n && s[ps->i] == '?') {
    size_t j = ps->i + 1;
    /* (?P<name>...) and (?<name>...) */
    size_t lt = 0;
    if (j + 1 < ps->n && s[j] == 'P' && s[j + 1] == '<') lt = j + 1;
    if (j + 1 < ps->n && s[j] == '<' && s[j + 1] != '=' && s[j + 1] != '!') lt = j;
    if (lt) {
      size_t k = lt + 1;
      while (k < ps->n && s[k] != '>') {
        if (!rx_is_word(s[k])) rx_error(ps, start, "invalid capture group name");
        k++;
      }
      if (k >= ps->n || k == lt + 1) rx_error(ps, start, "invalid capture group name");
      const uint8_t *name = s + lt + 1;
      size_t len = k - lt - 1;
      for (int g = 1; g <= ps->ncap && g < ps->namecap; g++) {
        if (ps->name[g] && ps->namelen[g] == len && memcmp(ps->name[g], name, len) == 0)
          rx_error(ps, start, "duplicate capture group name %.*s", (int)len, (const char *)name);
      }
      int cap = ++ps->ncap;
      rx_add_name(ps, cap, name, len);
      ps->i = k + 1;
      rx_node *x = rx_new(ps, N_CAP);
      x->cap = cap;
      rx_node *body = rx_parse_alts(ps, *fl, (long)start);
      ps->i++;
      x->subs = rx_alloc(&ps->ar, sizeof(rx_node *));
      x->subs[0] = body;
      return x;
    }
    if (j < ps->n && s[j] == 'P') {
      if (j + 1 < ps->n && (s[j + 1] == '=' || s[j + 1] == '>')) rx_error(ps, start, "backreferences are not supported");
      rx_error(ps, start, "invalid or unsupported group syntax");
    }
    if (j < ps->n && s[j] == '<') rx_error(ps, start, "lookbehind is not supported");
    /* Flags: (?i), (?i-s), (?i:...). */
    int nfl = *fl;
    bool neg = false, any = false;
    for (;;) {
      if (j >= ps->n) rx_error(ps, start, "missing closing ')'");
      int c = s[j];
      int bit = c == 'i' ? RX_FOLD : c == 'm' ? RX_MULTI : c == 's' ? RX_DOTNL : c == 'U' ? RX_UNGREEDY : 0;
      if (bit) {
        nfl = neg ? (nfl & ~bit) : (nfl | bit);
        any = true;
        j++;
        continue;
      }
      if (c == '-') {
        if (neg) rx_error(ps, start, "invalid flags group");
        neg = true;
        any = false;
        j++;
        continue;
      }
      if (c == ':' || c == ')') {
        if (!any && (neg || c == ')')) rx_error(ps, start, "invalid flags group");
        if (c == ')') {
          *fl = nfl;
          ps->i = j + 1;
          return NULL;
        }
        ps->i = j + 1;
        rx_node *body = rx_parse_alts(ps, nfl, (long)start);
        ps->i++;
        return body;
      }
      if (c == '=' || c == '!') rx_error(ps, start, "lookahead is not supported");
      rx_error(ps, start, "invalid or unsupported group syntax");
    }
  }
  int cap = ++ps->ncap;
  rx_node *x = rx_new(ps, N_CAP);
  x->cap = cap;
  rx_node *body = rx_parse_alts(ps, *fl, (long)start);
  ps->i++;
  x->subs = rx_alloc(&ps->ar, sizeof(rx_node *));
  x->subs[0] = body;
  return x;
}

/* An escape outside a class; the cursor is on the backslash. */
static rx_node *rx_parse_escape(rx_parser *ps, int fl, rx_nodes *items) {
  size_t start = ps->i;
  ps->i++;
  if (ps->i >= ps->n) rx_error(ps, start, "trailing backslash");
  int c = ps->s[ps->i];
  switch (c) {
  case 'A': ps->i++; return rx_assert(ps, RX_BOT);
  case 'z': ps->i++; return rx_assert(ps, RX_EOT);
  case 'b': ps->i++; return rx_assert(ps, RX_WB);
  case 'B': ps->i++; return rx_assert(ps, RX_NWB);
  case 'd': case 'D': case 's': case 'S': case 'w': case 'W': {
    ps->i++;
    rx_cls cl = {0};
    rx_perl_class(ps, &cl, c, (fl & RX_FOLD) != 0);
    return rx_class_node(ps, &cl);
  }
  case 'p': case 'P': {
    rx_cls cl = {0};
    rx_unicode_class(ps, &cl, (fl & RX_FOLD) != 0);
    return rx_class_node(ps, &cl);
  }
  case 'Q': {
    /* Literal text up to \E or the end. */
    ps->i++;
    rx_node *last = NULL;
    while (ps->i < ps->n) {
      if (ps->s[ps->i] == '\\' && ps->i + 1 < ps->n && ps->s[ps->i + 1] == 'E') {
        ps->i += 2;
        break;
      }
      if (last) rx_nodes_push(ps, items, last);
      last = rx_literal(ps, rx_next_rune(ps), fl);
    }
    return last ? last : rx_new(ps, N_EMPTY);
  }
  default: return rx_literal(ps, rx_escape_rune(ps, start), fl);
  }
}

/* Alternatives up to a ')' (left for the caller) or the end. open is the
 * offset of the group's '(', or -1 at the top. */
static rx_node *rx_parse_alts(rx_parser *ps, int fl, long open) {
  if (++ps->depth > RX_MAX_DEPTH) rx_error(ps, open < 0 ? 0 : (size_t)open, "expression nests too deeply");
  rx_nodes alts = {0}, items = {0};
  bool lastrep = false;
  for (;;) {
    if (ps->i >= ps->n) {
      if (open >= 0) rx_error(ps, (size_t)open, "missing closing ')'");
      break;
    }
    int c = ps->s[ps->i];
    if (c == ')') {
      if (open < 0) rx_error(ps, ps->i, "unexpected ')'");
      break;
    }
    if (c == '|') {
      rx_nodes_push(ps, &alts, rx_join(ps, N_CAT, &items));
      items = (rx_nodes){0};
      ps->i++;
      lastrep = false;
      continue;
    }
    int min = 0, max = 0;
    size_t end = 0;
    if (c == '*' || c == '+' || c == '?' || (c == '{' && rx_is_repeat(ps, &min, &max, &end))) {
      size_t op = ps->i;
      if (c == '*') { min = 0; max = -1; end = op + 1; }
      if (c == '+') { min = 1; max = -1; end = op + 1; }
      if (c == '?') { min = 0; max = 1; end = op + 1; }
      size_t oplen = end - op;
      if (items.n == 0) rx_error(ps, op, "missing argument to repetition operator %.*s", (int)oplen, (const char *)(ps->s + op));
      if (lastrep) rx_error(ps, op, "invalid nested repetition operator %.*s", (int)oplen, (const char *)(ps->s + op));
      if (min < 0 || min > RX_MAX_REPEAT || max > RX_MAX_REPEAT || (max >= 0 && min > max))
        rx_error(ps, op, "invalid repeat count %.*s", (int)oplen, (const char *)(ps->s + op));
      ps->i = end;
      bool greedy = true;
      if (ps->i < ps->n && ps->s[ps->i] == '?') {
        greedy = false;
        ps->i++;
      }
      if (fl & RX_UNGREEDY) greedy = !greedy;
      rx_node *x = rx_new(ps, N_REPEAT);
      x->min = min;
      x->max = max;
      x->greedy = greedy;
      x->subs = rx_alloc(&ps->ar, sizeof(rx_node *));
      x->subs[0] = items.v[items.n - 1];
      items.v[items.n - 1] = x;
      lastrep = true;
      continue;
    }
    lastrep = false;
    rx_node *x;
    switch (c) {
    case '(':
      x = rx_parse_group(ps, &fl);
      if (!x) continue;
      break;
    case '[': x = rx_parse_class(ps, fl); break;
    case '.': {
      ps->i++;
      rx_cls cl = {0};
      if (fl & RX_DOTNL) {
        rx_cls_push(ps, &cl, 0, RX_MAX_RUNE);
      } else {
        rx_cls_push(ps, &cl, 0, '\n' - 1);
        rx_cls_push(ps, &cl, '\n' + 1, RX_MAX_RUNE);
      }
      x = rx_class_node(ps, &cl);
      break;
    }
    case '^': ps->i++; x = rx_assert(ps, (fl & RX_MULTI) ? RX_BOL : RX_BOT); break;
    case '$': ps->i++; x = rx_assert(ps, (fl & RX_MULTI) ? RX_EOL : RX_EOT); break;
    case '\\': x = rx_parse_escape(ps, fl, &items); break;
    default: x = rx_literal(ps, rx_next_rune(ps), fl); break;
    }
    rx_nodes_push(ps, &items, x);
  }
  rx_nodes_push(ps, &alts, rx_join(ps, N_CAT, &items));
  ps->depth--;
  return rx_join(ps, N_ALT, &alts);
}

/* ---------------------------------------------------------- programs */

enum { OP_BYTE, OP_SET, OP_SPLIT, OP_NOP, OP_SAVE, OP_ASSERT, OP_MATCH, OP_FAIL };

typedef struct {
  uint8_t op, lo, hi, arg; /* OP_BYTE: lo-hi; OP_ASSERT: arg is the kind */
  int32_t x, y;            /* x: next (preferred); y: OP_SPLIT's other, OP_SET's set, OP_SAVE's slot */
} rx_inst;

typedef struct {
  rx_inst *in;
  int n, cap;
  uint8_t (*set)[32];
  int nset, setcap;
  int start;
} rx_prog;

typedef struct {
  rx_parser *ps;
  rx_prog *pg;
  bool rev; /* the reversed program: no captures, sequences reversed */
} rx_comp;

static int rx_emit(rx_comp *cc, int op, int x, int y) {
  rx_prog *pg = cc->pg;
  if (pg->n >= RX_MAX_INST) rx_error(cc->ps, 0, "expression too large");
  if (pg->n == pg->cap) {
    pg->cap = pg->cap ? pg->cap * 2 : 64;
    pg->in = sd_realloc(pg->in, (size_t)pg->cap * sizeof *pg->in);
  }
  rx_inst *in = &pg->in[pg->n];
  memset(in, 0, sizeof *in);
  in->op = (uint8_t)op;
  in->x = x;
  in->y = y;
  return pg->n++;
}

static int rx_emit_byte(rx_comp *cc, int lo, int hi, int next) {
  int pc = rx_emit(cc, OP_BYTE, next, 0);
  cc->pg->in[pc].lo = (uint8_t)lo;
  cc->pg->in[pc].hi = (uint8_t)hi;
  return pc;
}

static int rx_add_set(rx_comp *cc, const uint8_t set[32]) {
  rx_prog *pg = cc->pg;
  for (int k = 0; k < pg->nset; k++)
    if (memcmp(pg->set[k], set, 32) == 0) return k;
  if (pg->nset == pg->setcap) {
    pg->setcap = pg->setcap ? pg->setcap * 2 : 8;
    pg->set = sd_realloc(pg->set, (size_t)pg->setcap * 32);
  }
  memcpy(pg->set[pg->nset], set, 32);
  return pg->nset++;
}

static bool rx_nullable(const rx_node *x) {
  switch (x->kind) {
  case N_EMPTY:
  case N_ASSERT: return true;
  case N_CLASS: return false;
  case N_CAT:
    for (int k = 0; k < x->n; k++)
      if (!rx_nullable(x->subs[k])) return false;
    return true;
  case N_ALT:
    for (int k = 0; k < x->n; k++)
      if (rx_nullable(x->subs[k])) return true;
    return false;
  case N_REPEAT: return x->min == 0 || rx_nullable(x->subs[0]);
  default: return rx_nullable(x->subs[0]);
  }
}

/* UTF-8 sequences of a rune range: calls back with up to 4 byte ranges. */
typedef struct {
  uint8_t lo[4], hi[4];
  int n;
} rx_seq;

typedef struct {
  rx_seq *v;
  int n, cap;
} rx_seqs;

static void rx_utf8_encode(int32_t r, uint8_t *b) {
  if (r < 0x80) {
    b[0] = (uint8_t)r;
  } else if (r < 0x800) {
    b[0] = (uint8_t)(0xC0 | (r >> 6));
    b[1] = (uint8_t)(0x80 | (r & 0x3F));
  } else if (r < 0x10000) {
    b[0] = (uint8_t)(0xE0 | (r >> 12));
    b[1] = (uint8_t)(0x80 | ((r >> 6) & 0x3F));
    b[2] = (uint8_t)(0x80 | (r & 0x3F));
  } else {
    b[0] = (uint8_t)(0xF0 | (r >> 18));
    b[1] = (uint8_t)(0x80 | ((r >> 12) & 0x3F));
    b[2] = (uint8_t)(0x80 | ((r >> 6) & 0x3F));
    b[3] = (uint8_t)(0x80 | (r & 0x3F));
  }
}

static int rx_utf8_len(int32_t r) { return r < 0x80 ? 1 : r < 0x800 ? 2 : r < 0x10000 ? 3 : 4; }

static void rx_seqs_push(rx_parser *ps, rx_seqs *l, const rx_seq *s) {
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 16;
    rx_seq *v = rx_alloc(&ps->ar, (size_t)cap * sizeof *v);
    if (l->n) memcpy(v, l->v, (size_t)l->n * sizeof *v);
    l->v = v;
    l->cap = cap;
  }
  l->v[l->n++] = *s;
}

/* Splits lo-hi (no surrogates, same encoded length) until each piece is a
 * product of byte ranges (the utf8-ranges algorithm). */
static void rx_utf8_split(rx_parser *ps, rx_seqs *out, int32_t lo, int32_t hi) {
  static const int32_t limits[] = {0x7F, 0x7FF, 0xFFFF};
  for (int k = 0; k < 3; k++) {
    if (lo <= limits[k] && hi > limits[k]) {
      rx_utf8_split(ps, out, lo, limits[k]);
      rx_utf8_split(ps, out, limits[k] + 1, hi);
      return;
    }
  }
  int n = rx_utf8_len(lo);
  for (int i = 1; i < n; i++) {
    int32_t m = (1 << (6 * i)) - 1;
    if ((lo & ~m) != (hi & ~m)) {
      if ((lo & m) != 0) {
        rx_utf8_split(ps, out, lo, lo | m);
        rx_utf8_split(ps, out, (lo | m) + 1, hi);
        return;
      }
      if ((hi & m) != m) {
        rx_utf8_split(ps, out, lo, (hi & ~m) - 1);
        rx_utf8_split(ps, out, hi & ~m, hi);
        return;
      }
    }
  }
  rx_seq s;
  rx_utf8_encode(lo, s.lo);
  rx_utf8_encode(hi, s.hi);
  s.n = n;
  rx_seqs_push(ps, out, &s);
}

/* A trie of byte ranges for the forward program: sequences that share a
 * prefix share its instructions. */
typedef struct rx_trie rx_trie;
typedef struct {
  uint8_t lo, hi;
  rx_trie *child; /* NULL: the sequence ends here */
} rx_edge;
struct rx_trie {
  rx_edge *e;
  int n, cap;
};

static void rx_trie_insert(rx_parser *ps, rx_trie *t, const rx_seq *s) {
  for (int k = 0; k < s->n; k++) {
    bool last = k == s->n - 1;
    rx_edge *found = NULL;
    for (int j = 0; j < t->n; j++) {
      if (t->e[j].lo == s->lo[k] && t->e[j].hi == s->hi[k] && (t->e[j].child == NULL) == last) {
        found = &t->e[j];
        break;
      }
    }
    if (!found || last) {
      if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 4;
        rx_edge *e = rx_alloc(&ps->ar, (size_t)cap * sizeof *e);
        if (t->n) memcpy(e, t->e, (size_t)t->n * sizeof *e);
        t->e = e;
        t->cap = cap;
      }
      found = &t->e[t->n++];
      found->lo = s->lo[k];
      found->hi = s->hi[k];
      found->child = NULL;
      if (!last) {
        found->child = rx_alloc(&ps->ar, sizeof(rx_trie));
        memset(found->child, 0, sizeof(rx_trie));
      }
    }
    if (last) return;
    t = found->child;
  }
}

static int rx_alts(rx_comp *cc, const int *pcs, int n) {
  if (n == 0) return rx_emit(cc, OP_FAIL, 0, 0);
  int e = pcs[n - 1];
  for (int k = n - 2; k >= 0; k--) e = rx_emit(cc, OP_SPLIT, pcs[k], e);
  return e;
}

static int rx_emit_trie(rx_comp *cc, const rx_trie *t, int next) {
  int *pcs = rx_alloc(&cc->ps->ar, (size_t)t->n * sizeof(int));
  for (int j = 0; j < t->n; j++) {
    int to = t->e[j].child ? rx_emit_trie(cc, t->e[j].child, next) : next;
    pcs[j] = rx_emit_byte(cc, t->e[j].lo, t->e[j].hi, to);
  }
  return rx_alts(cc, pcs, t->n);
}

static int rx_compile_class(rx_comp *cc, const rx_node *x, int next) {
  rx_parser *ps = cc->ps;
  uint8_t set[32] = {0};
  bool anyset = false, fffd = false;
  rx_seqs seqs = {0};
  for (int k = 0; k < x->n; k++) {
    int32_t lo = x->pairs[2 * k], hi = x->pairs[2 * k + 1];
    if (lo <= 0xFFFD && hi >= 0xFFFD) fffd = true;
    for (int32_t b = lo; b <= hi && b < 0x80; b++) {
      set[b >> 3] |= (uint8_t)(1 << (b & 7));
      anyset = true;
    }
    if (hi < 0x80) continue;
    if (lo < 0x80) lo = 0x80;
    /* Surrogates have no UTF-8 form. */
    if (lo < 0xD800 && hi > 0xDFFF) {
      rx_utf8_split(ps, &seqs, lo, 0xD7FF);
      rx_utf8_split(ps, &seqs, 0xE000, hi);
    } else if (hi < 0xD800 || lo > 0xDFFF) {
      rx_utf8_split(ps, &seqs, lo, hi);
    } else {
      if (lo < 0xD800) rx_utf8_split(ps, &seqs, lo, 0xD7FF);
      if (hi > 0xDFFF) rx_utf8_split(ps, &seqs, 0xE000, hi);
    }
  }
  /* A byte that cannot start a UTF-8 sequence reads as U+FFFD, as in Go. */
  if (fffd) {
    for (int b = 0x80; b <= 0xFF; b++) {
      if ((b >= 0x80 && b <= 0xC1) || b >= 0xF5) {
        set[b >> 3] |= (uint8_t)(1 << (b & 7));
        anyset = true;
      }
    }
  }
  int *pcs = rx_alloc(&ps->ar, (size_t)(seqs.n + 2) * sizeof(int));
  int np = 0;
  if (anyset) {
    /* One contiguous range is an OP_BYTE, anything else an OP_SET. */
    int lo = -1, hi = -1, runs = 0;
    for (int b = 0; b < 256; b++) {
      bool in = (set[b >> 3] >> (b & 7)) & 1;
      if (in && (b == 0 || !((set[(b - 1) >> 3] >> ((b - 1) & 7)) & 1))) {
        runs++;
        lo = b;
      }
      if (in) hi = b;
    }
    if (runs == 1) {
      pcs[np++] = rx_emit_byte(cc, lo, hi, next);
    } else {
      pcs[np++] = rx_emit(cc, OP_SET, next, rx_add_set(cc, set));
    }
  }
  if (seqs.n > 0) {
    if (!cc->rev) {
      rx_trie root = {0};
      for (int k = 0; k < seqs.n; k++) rx_trie_insert(ps, &root, &seqs.v[k]);
      pcs[np++] = rx_emit_trie(cc, &root, next);
    } else {
      int *rp = rx_alloc(&ps->ar, (size_t)seqs.n * sizeof(int));
      for (int k = 0; k < seqs.n; k++) {
        int e = next;
        for (int j = 0; j < seqs.v[k].n; j++) e = rx_emit_byte(cc, seqs.v[k].lo[j], seqs.v[k].hi[j], e);
        rp[k] = e;
      }
      pcs[np++] = rx_alts(cc, rp, seqs.n);
    }
  }
  return rx_alts(cc, pcs, np);
}

static int rx_compile(rx_comp *cc, const rx_node *x, int next);

/* x+ : the body, then a split back to it (Go's loop). */
static int rx_plus(rx_comp *cc, const rx_node *x, bool greedy, int next) {
  int l = rx_emit(cc, OP_SPLIT, 0, 0);
  int body = rx_compile(cc, x, l);
  cc->pg->in[l].x = greedy ? body : next;
  cc->pg->in[l].y = greedy ? next : body;
  return body;
}

static int rx_quest(rx_comp *cc, int body, bool greedy, int next) {
  return greedy ? rx_emit(cc, OP_SPLIT, body, next) : rx_emit(cc, OP_SPLIT, next, body);
}

/* x* : (x+)? when x can match empty (Go's compiler, golang.org/issue/46123),
 * else a split in front of the body that loops back to it. */
static int rx_star(rx_comp *cc, const rx_node *x, bool greedy, int next) {
  if (rx_nullable(x)) return rx_quest(cc, rx_plus(cc, x, greedy, next), greedy, next);
  int l = rx_emit(cc, OP_SPLIT, 0, 0);
  int body = rx_compile(cc, x, l);
  cc->pg->in[l].x = greedy ? body : next;
  cc->pg->in[l].y = greedy ? next : body;
  return l;
}

static int rx_compile(rx_comp *cc, const rx_node *x, int next) {
  switch (x->kind) {
  case N_EMPTY: return next;
  case N_ASSERT: {
    int pc = rx_emit(cc, OP_ASSERT, next, 0);
    cc->pg->in[pc].arg = x->akind;
    return pc;
  }
  case N_CLASS: return rx_compile_class(cc, x, next);
  case N_CAT:
    if (cc->rev) {
      for (int k = 0; k < x->n; k++) next = rx_compile(cc, x->subs[k], next);
    } else {
      for (int k = x->n - 1; k >= 0; k--) next = rx_compile(cc, x->subs[k], next);
    }
    return next;
  case N_ALT: {
    int e = rx_compile(cc, x->subs[x->n - 1], next);
    for (int k = x->n - 2; k >= 0; k--) e = rx_emit(cc, OP_SPLIT, rx_compile(cc, x->subs[k], next), e);
    return e;
  }
  case N_CAP: {
    if (cc->rev) return rx_compile(cc, x->subs[0], next);
    int close = rx_emit(cc, OP_SAVE, next, 2 * x->cap + 1);
    int body = rx_compile(cc, x->subs[0], close);
    return rx_emit(cc, OP_SAVE, body, 2 * x->cap);
  }
  default: { /* N_REPEAT, expanded as Go's syntax.Simplify does */
    const rx_node *s = x->subs[0];
    int min = x->min, max = x->max;
    bool g = x->greedy;
    if (max == -1) {
      if (min == 0) return rx_star(cc, s, g, next);
      /* x{n,} is n-1 copies of x, then x+. */
      int e = rx_plus(cc, s, g, next);
      for (int k = 1; k < min; k++) e = rx_compile(cc, s, e);
      return e;
    }
    if (min == 0 && max == 0) return next;
    if (min == 1 && max == 1) return rx_compile(cc, s, next);
    /* x{n,m} is n copies of x, then (x(x(x)?)?)? with m-n x's. */
    int e = next;
    for (int k = 0; k < max - min; k++) e = rx_quest(cc, rx_compile(cc, s, e), g, next);
    for (int k = 0; k < min; k++) e = rx_compile(cc, s, e);
    return e;
  }
  }
}

/* Whether every match must start at the beginning of the text. */
static bool rx_anchored(const rx_node *x) {
  switch (x->kind) {
  case N_ASSERT: return x->akind == RX_BOT;
  case N_CAT: return x->n > 0 && rx_anchored(x->subs[0]);
  case N_ALT:
    for (int k = 0; k < x->n; k++)
      if (!rx_anchored(x->subs[k])) return false;
    return true;
  case N_CAP: return rx_anchored(x->subs[0]);
  case N_REPEAT: return x->min >= 1 && rx_anchored(x->subs[0]);
  default: return false;
  }
}

/* -------------------------------------------------------------- DFA */

enum { ST_MATCH = 1, ST_DEAD = 2 };
#define RX_CUT 0x20

typedef struct {
  uint32_t hash;
  uint16_t ctx;  /* the read side's rx_side | dec << 2 | RX_CUT */
  uint8_t flags; /* ST_MATCH: a match ended just before the last byte read */
  int8_t end[4]; /* whether a match ends at the end of the scan, by the other side; -1: not known yet */
  int32_t nq;
  int32_t *q;     /* the threads, as program counters, in priority order */
  int32_t next[]; /* by byte class: the next state, or -1 */
} rx_state;

typedef struct rx_re rx_re;

typedef struct {
  const rx_prog *pg;
  const rx_re *re;
  bool longest; /* the reverse DFA: longest match, no priorities, no new threads */
  rx_state **st;
  int nst, capst;
  int32_t *tab; /* hash table: state index + 1, or 0 */
  uint32_t tabmask;
  size_t mem;
  uint32_t gen;
  uint32_t *seen, *qseen;
  int32_t *stack, *list, *nq, *saveq;
  int nlist;
  int32_t starts[256];
} rx_dfa;

struct rx_re {
  int64_t id;
  uint64_t lru;
  rx_prog fwd, rev;
  int ngroups;
  bool anchored;
  uint8_t classof[256], rep[256];
  int ncls;
  /* the first bytes of every match, when they are few (a fast skip) */
  bool accel;
  int nfirst;
  uint8_t first1;
  bool first[256];
  rx_dfa *df, *dr;
  /* Pike VM scratch */
  int32_t *pk_sparse[2], *pk_dense[2];
  int64_t *pk_caps[2];
  int64_t *pk_stack;
  int64_t *pk_cur;
};

static void rx_dfa_flush(rx_dfa *d) {
  for (int k = 0; k < d->nst; k++) free(d->st[k]);
  d->nst = 0;
  memset(d->tab, 0, (size_t)(d->tabmask + 1) * sizeof(int32_t));
  for (int k = 0; k < 256; k++) d->starts[k] = -1;
  d->mem = 0;
}

static rx_dfa *rx_dfa_new(const rx_re *re, const rx_prog *pg, bool longest) {
  rx_dfa *d = sd_alloc(sizeof *d);
  memset(d, 0, sizeof *d);
  d->pg = pg;
  d->re = re;
  d->longest = longest;
  d->tabmask = 1023;
  d->tab = rx_zalloc((d->tabmask + 1) * sizeof(int32_t));
  size_t n = (size_t)pg->n + 1;
  d->seen = rx_zalloc(n * sizeof(uint32_t));
  d->qseen = rx_zalloc(n * sizeof(uint32_t));
  d->stack = sd_alloc(2 * n * sizeof(int32_t));
  d->list = sd_alloc(n * sizeof(int32_t));
  d->nq = sd_alloc(n * sizeof(int32_t));
  d->saveq = sd_alloc(n * sizeof(int32_t));
  for (int k = 0; k < 256; k++) d->starts[k] = -1;
  return d;
}

static void rx_dfa_free(rx_dfa *d) {
  if (!d) return;
  for (int k = 0; k < d->nst; k++) free(d->st[k]);
  free(d->st);
  free(d->tab);
  free(d->seen);
  free(d->qseen);
  free(d->stack);
  free(d->list);
  free(d->nq);
  free(d->saveq);
  free(d);
}

/* Follows the empty transitions from the threads q (and, with restart, a
 * new thread at the program's start, last) at a position with left and
 * right on its sides. The threads that read a byte go to d->list, in
 * priority order. Returns whether a match ends here; without longest, the
 * threads after the matching one are cut (leftmost-first). */
static bool rx_closure(rx_dfa *d, const int32_t *q, int nq, bool restart, int left, int right) {
  const rx_inst *in = d->pg->in;
  uint32_t gen = ++d->gen;
  if (gen == 0) {
    memset(d->seen, 0, ((size_t)d->pg->n + 1) * sizeof(uint32_t));
    memset(d->qseen, 0, ((size_t)d->pg->n + 1) * sizeof(uint32_t));
    gen = d->gen = 1;
  }
  d->nlist = 0;
  bool matched = false;
  for (int k = 0; k <= nq; k++) {
    int32_t pc0;
    if (k < nq) {
      pc0 = q[k];
    } else {
      if (!restart) break;
      pc0 = d->pg->start;
    }
    int sp = 0;
    d->stack[sp++] = pc0;
    while (sp > 0) {
      int32_t pc = d->stack[--sp];
      if (d->seen[pc] == gen) continue;
      d->seen[pc] = gen;
      const rx_inst *i = &in[pc];
      switch (i->op) {
      case OP_BYTE:
      case OP_SET: d->list[d->nlist++] = pc; break;
      case OP_MATCH:
        matched = true;
        if (!d->longest) return true;
        break;
      case OP_SPLIT:
        d->stack[sp++] = i->y;
        d->stack[sp++] = i->x;
        break;
      case OP_NOP:
      case OP_SAVE: d->stack[sp++] = i->x; break;
      case OP_ASSERT:
        if (rx_assert_ok(i->arg, left, right)) d->stack[sp++] = i->x;
        break;
      default: break;
      }
    }
  }
  return matched;
}

static int rx_q_cmp(const void *a, const void *b) {
  int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
  return x < y ? -1 : x > y;
}

static uint32_t rx_state_hash(int ctx, int flags, const int32_t *q, int nq) {
  uint32_t h = 2166136261u;
  h = (h ^ (uint32_t)ctx) * 16777619u;
  h = (h ^ (uint32_t)flags) * 16777619u;
  for (int k = 0; k < nq; k++) h = (h ^ (uint32_t)q[k]) * 16777619u;
  return h;
}

/* The index of the state (ctx, flags, q), made if needed. -1 when the
 * cache is over its budget (the caller flushes it). */
static int rx_state_find(rx_dfa *d, int ctx, int flags, const int32_t *q, int nq, bool force) {
  uint32_t h = rx_state_hash(ctx, flags, q, nq);
  for (uint32_t k = h & d->tabmask;; k = (k + 1) & d->tabmask) {
    int32_t e = d->tab[k];
    if (e == 0) break;
    rx_state *s = d->st[e - 1];
    if (s->hash == h && s->ctx == ctx && s->flags == flags && s->nq == nq && memcmp(s->q, q, (size_t)nq * sizeof(int32_t)) == 0)
      return e - 1;
  }
  int ncls = d->re->ncls;
  size_t size = sizeof(rx_state) + (size_t)(ncls + nq) * sizeof(int32_t);
  if (!force && d->mem + size + 64 > RX_DFA_BUDGET) return -1;
  rx_state *s = sd_alloc(size);
  s->hash = h;
  s->ctx = (uint16_t)ctx;
  s->flags = (uint8_t)flags;
  memset(s->end, -1, sizeof s->end);
  s->nq = nq;
  for (int k = 0; k < ncls; k++) s->next[k] = -1;
  s->q = s->next + ncls;
  memcpy(s->q, q, (size_t)nq * sizeof(int32_t));
  if (d->nst == d->capst) {
    d->capst = d->capst ? d->capst * 2 : 64;
    d->st = sd_realloc(d->st, (size_t)d->capst * sizeof *d->st);
  }
  d->st[d->nst++] = s;
  d->mem += size + 64;
  /* Grow the table at half full. */
  if ((uint32_t)d->nst * 2 > d->tabmask) {
    uint32_t mask = d->tabmask * 2 + 1;
    int32_t *tab = rx_zalloc((mask + 1) * sizeof(int32_t));
    for (int k = 0; k < d->nst; k++) {
      uint32_t j = d->st[k]->hash & mask;
      while (tab[j]) j = (j + 1) & mask;
      tab[j] = k + 1;
    }
    free(d->tab);
    d->tab = tab;
    d->tabmask = mask;
  } else {
    uint32_t j = h & d->tabmask;
    while (d->tab[j]) j = (j + 1) & d->tabmask;
    d->tab[j] = d->nst;
  }
  return d->nst - 1;
}

static int rx_state_get(rx_dfa *d, int ctx, int flags, const int32_t *q, int nq) {
  int k = rx_state_find(d, ctx, flags, q, nq, false);
  if (k >= 0) return k;
  rx_dfa_flush(d);
  return rx_state_find(d, ctx, flags, q, nq, true);
}

/* The state after state si reads a byte of class cls. */
static int rx_dfa_step(rx_dfa *d, int si, int cls) {
  rx_state *s = d->st[si];
  int c = d->re->rep[cls];
  int side = s->ctx & 3, dec = (s->ctx >> 2) & 7;
  bool cut = (s->ctx & RX_CUT) != 0;
  bool matched, boundary = true;
  if (!d->longest) {
    boundary = rx_boundary(dec, c);
    matched = rx_closure(d, s->q, s->nq, !cut && boundary, side, rx_side(c));
  } else {
    matched = rx_closure(d, s->q, s->nq, false, rx_side(c), side);
  }
  /* The threads that read c. */
  const rx_inst *in = d->pg->in;
  uint32_t gen = d->gen;
  int nq = 0;
  for (int k = 0; k < d->nlist; k++) {
    const rx_inst *i = &in[d->list[k]];
    bool ok = i->op == OP_BYTE ? (c >= i->lo && c <= i->hi) : ((d->pg->set[i->y][c >> 3] >> (c & 7)) & 1);
    if (ok && d->qseen[i->x] != gen) {
      d->qseen[i->x] = gen;
      d->nq[nq++] = i->x;
    }
  }
  int ctx, flags = matched ? ST_MATCH : 0;
  if (!d->longest) {
    bool ncut = cut || matched;
    ctx = rx_side(c) | (ncut ? RX_CUT : (rx_dec_after(dec, c, boundary) << 2));
    if (nq == 0 && ncut) flags |= ST_DEAD;
  } else {
    if (nq > 1) qsort(d->nq, (size_t)nq, sizeof(int32_t), rx_q_cmp);
    ctx = rx_side(c);
    if (nq == 0) flags |= ST_DEAD;
  }
  int k = rx_state_find(d, ctx, flags, d->nq, nq, false);
  if (k < 0) {
    /* Over budget: start again with only s and the new state. */
    int sctx = s->ctx, sflags = s->flags, snq = s->nq;
    memcpy(d->saveq, s->q, (size_t)snq * sizeof(int32_t));
    rx_dfa_flush(d);
    si = rx_state_find(d, sctx, sflags, d->saveq, snq, true);
    k = rx_state_find(d, ctx, flags, d->nq, nq, true);
    s = d->st[si];
  }
  s->next[cls] = k;
  return k;
}

/* Whether a match ends at the end of the scan, where the other side is
 * other (the edge of the text, or the byte before the reverse scan's
 * limit). */
static bool rx_dfa_end(rx_dfa *d, int si, int other) {
  rx_state *s = d->st[si];
  if (s->end[other] < 0) {
    bool m;
    if (!d->longest) {
      m = rx_closure(d, s->q, s->nq, !(s->ctx & RX_CUT), s->ctx & 3, other);
    } else {
      m = rx_closure(d, s->q, s->nq, false, other, s->ctx & 3);
    }
    s->end[other] = m;
  }
  return s->end[other] == 1;
}

/* The start state: forward, no threads yet (new ones start at each
 * boundary), or with only the program's start and no restarts
 * (anchored); reverse: the reversed program's start. side is what is
 * before (forward) or after (reverse) the starting position. */
static int rx_dfa_start(rx_dfa *d, bool anchored, int side) {
  int key = side | (anchored ? 4 : 0);
  if (d->starts[key] >= 0) return d->starts[key];
  int32_t q[1] = {d->pg->start};
  int k;
  if (d->longest) {
    k = rx_state_get(d, side, 0, q, 1);
  } else if (anchored) {
    k = rx_state_get(d, side | RX_CUT, 0, q, 1);
  } else {
    k = rx_state_get(d, side, 0, q, 0);
  }
  d->starts[key] = k;
  return k;
}

/* Where the leftmost-first match that starts at or after p ends, or -1.
 * With earliest, the first position where any match ends (enough to know
 * that there is one). */
static int64_t rx_scan_forward(rx_re *re, const uint8_t *t, int64_t n, int64_t p, bool earliest) {
  if (re->anchored && p > 0) return -1;
  if (!re->df) re->df = rx_dfa_new(re, &re->fwd, false);
  rx_dfa *d = re->df;
  const uint8_t *classof = re->classof;
  int si = rx_dfa_start(d, re->anchored, rx_side(p > 0 ? t[p - 1] : -1));
  rx_state *s = d->st[si];
  int64_t last = -1;
  bool accel = re->accel && !re->anchored;
  for (int64_t i = p; i < n; i++) {
    if (accel && s->nq == 0 && !(s->ctx & RX_CUT)) {
      /* No thread is alive: skip to the next byte that can start a match. */
      int64_t j = i;
      if (re->nfirst == 1) {
        const uint8_t *f = memchr(t + i, re->first1, (size_t)(n - i));
        j = f ? f - t : n;
      } else {
        while (j < n && !re->first[t[j]]) j++;
      }
      if (j >= n) return last;
      if (j > i) {
        i = j;
        si = rx_dfa_start(d, false, rx_side(t[j - 1]));
        s = d->st[si];
      }
    }
    int cls = classof[t[i]];
    int32_t nx = s->next[cls];
    if (nx < 0) nx = rx_dfa_step(d, si, cls);
    si = nx;
    s = d->st[si];
    if (s->flags) {
      if (s->flags & ST_MATCH) {
        last = i;
        if (earliest) return last;
      }
      if (s->flags & ST_DEAD) return last;
    }
  }
  if (rx_dfa_end(d, si, RX_EDGE)) last = n;
  return last;
}

/* Where the leftmost match that ends at e starts, scanning back to p. */
static int64_t rx_scan_reverse(rx_re *re, const uint8_t *t, int64_t n, int64_t e, int64_t p) {
  if (!re->dr) re->dr = rx_dfa_new(re, &re->rev, true);
  rx_dfa *d = re->dr;
  const uint8_t *classof = re->classof;
  int si = rx_dfa_start(d, true, rx_side(e < n ? t[e] : -1));
  rx_state *s = d->st[si];
  int64_t first = -1;
  for (int64_t i = e - 1; i >= p; i--) {
    int cls = classof[t[i]];
    int32_t nx = s->next[cls];
    if (nx < 0) nx = rx_dfa_step(d, si, cls);
    si = nx;
    s = d->st[si];
    if (s->flags) {
      /* Matches start only at character boundaries, as in Go. */
      if ((s->flags & ST_MATCH) && rx_at_boundary(t, n, i + 1)) first = i + 1;
      if (s->flags & ST_DEAD) return first;
    }
  }
  if (rx_dfa_end(d, si, rx_side(p > 0 ? t[p - 1] : -1))) first = p;
  return first;
}

/* ----------------------------------------------------------- Pike VM */

/* Adds the thread pc (and what it reaches without reading) to list l at
 * position pos, with capture slots caps (restored before returning). */
static void rx_pike_add(rx_re *re, int which, int32_t pc0, int64_t pos, int64_t *caps, int left, int right, int *ln) {
  const rx_inst *in = re->fwd.in;
  int32_t *sparse = re->pk_sparse[which], *dense = re->pk_dense[which];
  int64_t *lcaps = re->pk_caps[which];
  int ncap = 2 * (re->ngroups + 1);
  int64_t *stack = re->pk_stack;
  int sp = 0;
  stack[sp++] = pc0;
  while (sp > 0) {
    int64_t e = stack[--sp];
    if (e < 0) {
      /* restore: -(slot + 1), then the value */
      int64_t v = stack[--sp];
      caps[-e - 1] = v;
      continue;
    }
    int32_t pc = (int32_t)e;
    int32_t k = sparse[pc];
    if (k < *ln && dense[k] == pc) continue;
    sparse[pc] = *ln;
    dense[*ln] = pc;
    int slot = (*ln)++;
    const rx_inst *i = &in[pc];
    switch (i->op) {
    case OP_BYTE:
    case OP_SET:
    case OP_MATCH: memcpy(&lcaps[(size_t)slot * (size_t)ncap], caps, (size_t)ncap * sizeof(int64_t)); break;
    case OP_SPLIT:
      stack[sp++] = i->y;
      stack[sp++] = i->x;
      break;
    case OP_NOP: stack[sp++] = i->x; break;
    case OP_SAVE:
      if (i->y < ncap) {
        stack[sp++] = caps[i->y];
        stack[sp++] = -(int64_t)i->y - 1;
        caps[i->y] = pos;
      }
      stack[sp++] = i->x;
      break;
    case OP_ASSERT:
      if (rx_assert_ok(i->arg, left, right)) stack[sp++] = i->x;
      break;
    default: break;
    }
  }
}

/* The leftmost-first match starting at p (anchored) or at or after p, with
 * every group: out gets 2 * (ngroups + 1) positions, -1 for a group that
 * did not take part. */
static bool rx_pike(rx_re *re, const uint8_t *t, int64_t n, int64_t p, bool anchored, int64_t *out) {
  int ncap = 2 * (re->ngroups + 1);
  size_t ni = (size_t)re->fwd.n;
  if (!re->pk_stack) {
    for (int w = 0; w < 2; w++) {
      re->pk_sparse[w] = rx_zalloc(ni * sizeof(int32_t));
      re->pk_dense[w] = rx_zalloc(ni * sizeof(int32_t));
      re->pk_caps[w] = sd_alloc(ni * (size_t)ncap * sizeof(int64_t));
    }
    re->pk_stack = sd_alloc((3 * ni + 2) * sizeof(int64_t));
    re->pk_cur = sd_alloc((size_t)ncap * sizeof(int64_t));
  }
  int cur = 0, ncur = 0;
  bool matched = false;
  int dec = 0;
  for (int64_t i = p;; i++) {
    int c = i < n ? t[i] : -1;
    bool boundary = c < 0 || rx_boundary(dec, c);
    if (!matched && (anchored ? i == p : boundary)) {
      for (int k = 0; k < ncap; k++) re->pk_cur[k] = -1;
      rx_pike_add(re, cur, re->fwd.start, i, re->pk_cur, rx_side(i > 0 ? t[i - 1] : -1), rx_side(c), &ncur);
    }
    if (ncur == 0 && (matched || anchored || c < 0)) break;
    int nnext = 0, nxt = 1 - cur;
    int left2 = rx_side(c), right2 = rx_side(i + 1 < n ? t[i + 1] : -1);
    for (int k = 0; k < ncur; k++) {
      int32_t pc = re->pk_dense[cur][k];
      const rx_inst *in = &re->fwd.in[pc];
      int64_t *caps = &re->pk_caps[cur][(size_t)k * (size_t)ncap];
      if (in->op == OP_MATCH) {
        memcpy(out, caps, (size_t)ncap * sizeof(int64_t));
        matched = true;
        break;
      }
      if (c < 0 || (in->op != OP_BYTE && in->op != OP_SET)) continue;
      bool ok = in->op == OP_BYTE ? (c >= in->lo && c <= in->hi) : ((re->fwd.set[in->y][c >> 3] >> (c & 7)) & 1);
      if (ok) rx_pike_add(re, nxt, in->x, i + 1, caps, left2, right2, &nnext);
    }
    cur = nxt;
    ncur = nnext;
    if (c < 0) break;
    if (!anchored) dec = rx_dec_after(dec, c, boundary);
  }
  return matched;
}

/* ------------------------------------------------------- compilation */

static void rx_re_free(rx_re *re) {
  if (!re) return;
  free(re->fwd.in);
  free(re->fwd.set);
  free(re->rev.in);
  free(re->rev.set);
  rx_dfa_free(re->df);
  rx_dfa_free(re->dr);
  for (int w = 0; w < 2; w++) {
    free(re->pk_sparse[w]);
    free(re->pk_dense[w]);
    free(re->pk_caps[w]);
  }
  free(re->pk_stack);
  free(re->pk_cur);
  free(re);
}

static void rx_mark(bool *brk, int b) {
  if (b > 0 && b < 256) brk[b] = true;
}

static void rx_mark_prog(bool *brk, const rx_prog *pg) {
  for (int k = 0; k < pg->n; k++) {
    if (pg->in[k].op == OP_BYTE) {
      rx_mark(brk, pg->in[k].lo);
      rx_mark(brk, pg->in[k].hi + 1);
    }
  }
  for (int k = 0; k < pg->nset; k++) {
    for (int b = 1; b < 256; b++) {
      if (((pg->set[k][b >> 3] >> (b & 7)) & 1) != ((pg->set[k][(b - 1) >> 3] >> ((b - 1) & 7)) & 1)) brk[b] = true;
    }
  }
}

/* Byte classes: bytes that no instruction, assertion or UTF-8 boundary
 * tells apart share a class, and a DFA state has one transition per class. */
static void rx_byte_classes(rx_re *re) {
  bool brk[256] = {0};
  rx_mark_prog(brk, &re->fwd);
  rx_mark_prog(brk, &re->rev);
  static const int fixed[] = {'\n', '\n' + 1, '0', '9' + 1, 'A', 'Z' + 1, '_', '_' + 1, 'a', 'z' + 1, 0x80, 0x90,
                              0xA0, 0xC0, 0xC2, 0xE0, 0xE1, 0xED, 0xEE, 0xF0, 0xF1, 0xF4, 0xF5};
  for (size_t k = 0; k < sizeof fixed / sizeof fixed[0]; k++) rx_mark(brk, fixed[k]);
  int id = -1;
  for (int b = 0; b < 256; b++) {
    if (b == 0 || brk[b]) {
      id++;
      re->rep[id] = (uint8_t)b;
    }
    re->classof[b] = (uint8_t)id;
  }
  re->ncls = id + 1;
}

/* The bytes that can start a match, when no match can be empty and they
 * are few and never UTF-8 continuation bytes. */
static void rx_first_bytes(rx_re *re) {
  const rx_prog *pg = &re->fwd;
  bool *seen = rx_zalloc((size_t)pg->n);
  int32_t *stack = sd_alloc(2 * (size_t)pg->n * sizeof(int32_t) + 8);
  int sp = 0;
  bool ok = true;
  stack[sp++] = pg->start;
  memset(re->first, 0, sizeof re->first);
  while (sp > 0 && ok) {
    int32_t pc = stack[--sp];
    if (seen[pc]) continue;
    seen[pc] = true;
    const rx_inst *i = &pg->in[pc];
    switch (i->op) {
    case OP_BYTE:
      for (int b = i->lo; b <= i->hi; b++) re->first[b] = true;
      break;
    case OP_SET:
      for (int b = 0; b < 256; b++)
        if ((pg->set[i->y][b >> 3] >> (b & 7)) & 1) re->first[b] = true;
      break;
    case OP_MATCH: ok = false; break;
    case OP_SPLIT:
      stack[sp++] = i->y;
      stack[sp++] = i->x;
      break;
    case OP_NOP:
    case OP_SAVE:
    case OP_ASSERT: stack[sp++] = i->x; break;
    default: break;
    }
  }
  free(seen);
  free(stack);
  int count = 0;
  for (int b = 0; b < 256; b++) {
    if (!re->first[b]) continue;
    if (b >= 0x80 && b <= 0xBF) ok = false;
    re->first1 = (uint8_t)b;
    count++;
  }
  re->nfirst = count;
  re->accel = ok && count > 0 && count <= 32;
}

/* Parses and compiles pattern. On failure returns NULL with the reason in
 * msg. With names, also returns the groups' names ("" for group 0 and
 * unnamed groups). */
static rx_re *rx_build(const uint8_t *s, size_t n, char *msg, size_t msgsize, sd_list **names) {
  rx_parser *ps = sd_alloc(sizeof *ps);
  memset(ps, 0, sizeof *ps);
  ps->s = s;
  ps->n = n;
  rx_re *volatile re = NULL;
  if (setjmp(ps->fail)) {
    snprintf(msg, msgsize, "%s", ps->msg);
    rx_arena_free(&ps->ar);
    rx_re_free(re);
    free(ps);
    return NULL;
  }
  rx_node *root = rx_parse_alts(ps, 0, -1);
  re = sd_alloc(sizeof(rx_re));
  memset(re, 0, sizeof(rx_re));
  re->ngroups = ps->ncap;
  re->anchored = rx_anchored(root);
  rx_comp cc = {ps, &re->fwd, false};
  int match = rx_emit(&cc, OP_MATCH, 0, 0);
  int close = rx_emit(&cc, OP_SAVE, match, 1);
  int body = rx_compile(&cc, root, close);
  re->fwd.start = rx_emit(&cc, OP_SAVE, body, 0);
  rx_comp rc = {ps, &re->rev, true};
  int rmatch = rx_emit(&rc, OP_MATCH, 0, 0);
  re->rev.start = rx_compile(&rc, root, rmatch);
  rx_byte_classes(re);
  rx_first_bytes(re);
  if (names) {
    sd_list *l = sd_list_new(&sd_type_str, ps->ncap + 1);
    for (int g = 0; g <= ps->ncap; g++) {
      sd_str name = SD_STR_LIT("");
      if (g > 0 && g < ps->namecap && ps->name[g]) name = sd_str_new((const char *)ps->name[g], (int64_t)ps->namelen[g]);
      SD_LIST_PUSH(&l, sd_str, name);
    }
    *names = l;
  }
  rx_arena_free(&ps->ar);
  free(ps);
  return re;
}

/* ------------------------------------------------------------- cache */

typedef struct {
  rx_re *e[RX_CACHE_N];
  uint64_t tick;
  bool hooked;
} rx_cache;

static int rx_cache_key;

static void rx_cache_free(void) {
  rx_cache *c = sd_goroutine_local(&rx_cache_key, sizeof(rx_cache));
  for (int k = 0; k < RX_CACHE_N; k++) {
    rx_re_free(c->e[k]);
    c->e[k] = NULL;
  }
}

static void rx_cache_put(rx_cache *c, rx_re *re) {
  int victim = 0;
  for (int k = 0; k < RX_CACHE_N; k++) {
    if (!c->e[k]) {
      victim = k;
      break;
    }
    if (c->e[k]->lru < c->e[victim]->lru) victim = k;
  }
  rx_re_free(c->e[victim]);
  c->e[victim] = re;
  re->lru = ++c->tick;
}

static rx_cache *rx_cache_get(void) {
  rx_cache *c = sd_goroutine_local(&rx_cache_key, sizeof(rx_cache));
  if (!c->hooked) {
    sd_goroutine_atexit(rx_cache_free);
    c->hooked = true;
  }
  return c;
}

static int64_t rx_i64(sd_int v) {
  int64_t x = 0;
  if (!sd_int_to_i64(v, &x)) return -1;
  return x;
}

/* The compiled form of the regex with this id, compiled again if this
 * goroutine has not seen it (or has dropped it). */
static rx_re *rx_get(sd_int id, sd_str pattern) {
  int64_t idv = rx_i64(id);
  rx_cache *c = rx_cache_get();
  for (int k = 0; k < RX_CACHE_N; k++) {
    if (c->e[k] && c->e[k]->id == idv) {
      c->e[k]->lru = ++c->tick;
      return c->e[k];
    }
  }
  char msg[300];
  rx_re *re = rx_build((const uint8_t *)pattern->data, (size_t)pattern->len, msg, sizeof msg, NULL);
  if (!re) sd_crash("regex: pattern %s no longer compiles: %s", pattern->data, msg);
  re->id = idv;
  rx_cache_put(c, re);
  return re;
}

/* ----------------------------------------------------------- search */

/* The leftmost-first match at or after p: caps gets 2 * (ngroups + 1)
 * positions. */
static bool rx_search(rx_re *re, const uint8_t *t, int64_t n, int64_t p, int64_t *caps) {
  int64_t e = rx_scan_forward(re, t, n, p, false);
  if (e < 0) return false;
  int64_t s = rx_scan_reverse(re, t, n, e, p);
  if (s < 0) return rx_pike(re, t, n, p, false, caps); /* not expected */
  if (re->ngroups == 0) {
    caps[0] = s;
    caps[1] = e;
    return true;
  }
  if (!rx_pike(re, t, n, s, true, caps)) {
    caps[0] = s;
    caps[1] = e;
    for (int k = 2; k < 2 * (re->ngroups + 1); k++) caps[k] = -1;
  }
  return true;
}

static void rx_push_caps(sd_list **l, const int64_t *caps, int ncap) {
  for (int k = 0; k < ncap; k++) SD_LIST_PUSH(l, sd_int, SD_INT(caps[k]));
}

static _Atomic int64_t rx_next_id;

sd_int sd_lib_regex_rxNewId(void) { return sd_int_from_i64(atomic_fetch_add(&rx_next_id, 1) + 1); }

sd_err sd_lib_regex_rxCompile(sd_str pattern, sd_int id, sd_list **names) {
  char msg[300];
  rx_re *re = rx_build((const uint8_t *)pattern->data, (size_t)pattern->len, msg, sizeof msg, names);
  if (!re) return sd_errorf("regex.compile: %s: %s", pattern->data, msg);
  re->id = rx_i64(id);
  rx_cache_put(rx_cache_get(), re);
  return SD_ERR_NULL;
}

bool sd_lib_regex_rxMatches(sd_int id, sd_str pattern, sd_str text) {
  rx_re *re = rx_get(id, pattern);
  return rx_scan_forward(re, (const uint8_t *)text->data, text->len, 0, true) >= 0;
}

sd_list *sd_lib_regex_rxFind(sd_int id, sd_str pattern, sd_str text, sd_int start) {
  rx_re *re = rx_get(id, pattern);
  int ncap = 2 * (re->ngroups + 1);
  int64_t caps[64], *cp = ncap <= 64 ? caps : sd_alloc((size_t)ncap * sizeof(int64_t));
  sd_list *out = NULL;
  if (rx_search(re, (const uint8_t *)text->data, text->len, rx_i64(start), cp)) {
    out = sd_list_new(&sd_type_int, ncap);
    rx_push_caps(&out, cp, ncap);
  }
  if (cp != caps) free(cp);
  return out;
}

/* Every match, as Go's FindAll: an empty match right after the previous
 * match is skipped, and after an empty match the search goes on one
 * character further. */
sd_list *sd_lib_regex_rxFindAll(sd_int id, sd_str pattern, sd_str text) {
  rx_re *re = rx_get(id, pattern);
  const uint8_t *t = (const uint8_t *)text->data;
  int64_t n = text->len;
  int ncap = 2 * (re->ngroups + 1);
  int64_t caps[64], *cp = ncap <= 64 ? caps : sd_alloc((size_t)ncap * sizeof(int64_t));
  sd_list *out = sd_list_new(&sd_type_int, 0);
  int64_t prev = -1;
  for (int64_t pos = 0; pos <= n;) {
    if (!rx_search(re, t, n, pos, cp)) break;
    bool accept = true;
    if (cp[1] == pos) {
      if (cp[0] == prev) accept = false;
      pos += pos < n ? rx_width(t, n, pos) : 1;
    } else {
      pos = cp[1];
    }
    prev = cp[1];
    if (accept) rx_push_caps(&out, cp, ncap);
  }
  if (cp != caps) free(cp);
  return out;
}
