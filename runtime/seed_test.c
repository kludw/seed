/* seed test: the runner of test programs, and the values a failed assert
 * shows. */
#include "seed_internal.h"
#include <unistd.h>

static sd_str sd_assert_message(const void *self) { return sd_str_retain(*(sd_str const *)self); }

/* The vtable of the errors made by a failed assert, so that the runner can
 * tell them from the errors a test returns. */
const sd_error_vtable sd_assert_vtable = {{"assert"}, sd_assert_message};

sd_err sd_assert_error(sd_str msg) {
  sd_str m = sd_str_retain(msg);
  return sd_iface_new(&sd_assert_vtable.hdr, &sd_type_str, &m);
}

bool sd_err_is_assert(sd_err e) { return e.vt == &sd_assert_vtable.hdr; }

sd_str sd_str_quote(sd_str s) {
  size_t cap = (size_t)s->len * 4 + 3, n = 0;
  char *buf = sd_alloc(cap);
  buf[n++] = '"';
  for (int64_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    const char *esc = NULL;
    switch (c) {
    case '\n': esc = "\\n"; break;
    case '\t': esc = "\\t"; break;
    case '\r': esc = "\\r"; break;
    case '\\': esc = "\\\\"; break;
    case '"': esc = "\\\""; break;
    case 0: esc = "\\0"; break;
    default: break;
    }
    if (esc) {
      buf[n++] = esc[0];
      buf[n++] = esc[1];
    } else if (c < 32 || c == 127) {
      static const char hex[] = "0123456789abcdef";
      buf[n++] = '\\';
      buf[n++] = 'x';
      buf[n++] = hex[c >> 4];
      buf[n++] = hex[c & 15];
    } else {
      buf[n++] = (char)c;
    }
  }
  buf[n++] = '"';
  sd_str r = sd_str_new(buf, (int64_t)n);
  free(buf);
  return r;
}

sd_str sd_err_show(sd_err e) {
  if (sd_err_is_null(e)) return sd_str_from_cstr("null");
  sd_str m = sd_err_message(e);
  sd_str q = sd_str_quote(m);
  sd_str_release(m);
  return q;
}

/* ---- showing values (a failed assert) ----
 * Values become text close to how they are written in Seed: [1, 2],
 * {"a": 1}, User{name: "x", age: 3}, Shape.Circle(2.0), Dir.North, null.
 * The text is cut at a limit (ending in "..."), and so is the nesting, so
 * that a huge or deep value gives a line that can still be read. */

#define SHOW_LIMIT 200        /* bytes of one value in a failed assert */
#define SHOW_DIFF_LIMIT 120   /* bytes of each side of a difference */
#define SHOW_DEPTH 32

typedef struct {
  char *p;
  size_t n, cap, limit;
  bool cut;
} show_buf;

static void show_put(show_buf *b, const char *s, size_t n) {
  if (b->cut) return;
  if (b->n + n > b->limit) {
    n = b->limit - b->n;
    b->cut = true;
  }
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = sd_realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
}

static void show_cstr(show_buf *b, const char *s) { show_put(b, s, strlen(s)); }

static void show_owned(show_buf *b, sd_str s) {
  show_put(b, s->data, (size_t)s->len);
  sd_str_release(s);
}

static const char *show_type_name(const sd_type *t) { return t->show ? t->show : t->name; }

static void show_value(show_buf *b, const sd_type *t, const void *v, int depth);

static void show_fields(show_buf *b, const sd_field *fs, int32_t n, const char *const *names, const char *base, int depth) {
  for (int32_t i = 0; i < n && !b->cut; i++) {
    if (i > 0) show_cstr(b, ", ");
    if (names) {
      show_cstr(b, names[i]);
      show_cstr(b, ": ");
    }
    show_value(b, fs[i].type, base + fs[i].offset, depth + 1);
  }
}

static void show_value(show_buf *b, const sd_type *t, const void *v, int depth) {
  if (b->cut) return;
  if (depth > SHOW_DEPTH) {
    show_cstr(b, "...");
    b->cut = true;
    return;
  }
  switch (t->kind) {
  case SD_K_INT: {
    sd_int x = *(const sd_int *)v;
    if (sd_int_is_null(x)) show_cstr(b, "null");
    else show_owned(b, sd_int_to_str(x));
    return;
  }
  case SD_K_FLOAT: show_owned(b, sd_float_to_str(*(const double *)v)); return;
  case SD_K_BOOL: show_cstr(b, *(const bool *)v ? "true" : "false"); return;
  case SD_K_BYTE: show_owned(b, sd_byte_to_str(*(const uint8_t *)v)); return;
  case SD_K_OPT_FLOAT: {
    const sd_opt_float *o = v;
    if (o->has) show_owned(b, sd_float_to_str(o->v));
    else show_cstr(b, "null");
    return;
  }
  case SD_K_OPT_BOOL: {
    const sd_opt_bool *o = v;
    show_cstr(b, !o->has ? "null" : o->v ? "true" : "false");
    return;
  }
  case SD_K_OPT_BYTE: {
    const sd_opt_byte *o = v;
    if (o->has) show_owned(b, sd_byte_to_str(o->v));
    else show_cstr(b, "null");
    return;
  }
  case SD_K_FN: show_cstr(b, *(void *const *)v ? "fn" : "null"); return;
  case SD_K_CHAN: show_cstr(b, *(sd_chan *const *)v ? "chan" : "null"); return;
  case SD_K_STR: {
    sd_str s = *(sd_str const *)v;
    if (!s) show_cstr(b, "null");
    else show_owned(b, sd_str_quote(s));
    return;
  }
  case SD_K_ERR: show_owned(b, sd_err_show(*(const sd_err *)v)); return;
  case SD_K_LIST: {
    const sd_list *l = *(sd_list *const *)v;
    if (!l) {
      show_cstr(b, "null");
      return;
    }
    show_cstr(b, "[");
    const char *data = sd_list_data(l);
    for (int64_t i = 0; i < l->len && !b->cut; i++) {
      if (i > 0) show_cstr(b, ", ");
      show_value(b, l->et, data + (size_t)i * l->et->size, depth + 1);
    }
    show_cstr(b, "]");
    return;
  }
  case SD_K_MAP: {
    const sd_map *m = *(sd_map *const *)v;
    if (!m) {
      show_cstr(b, "null");
      return;
    }
    show_cstr(b, "{");
    int64_t pos = 0;
    void *k, *x;
    bool first = true;
    while (!b->cut && sd_map_next(m, &pos, &k, &x)) {
      if (!first) show_cstr(b, ", ");
      first = false;
      show_value(b, m->kt, k, depth + 1);
      show_cstr(b, ": ");
      show_value(b, m->vt, x, depth + 1);
    }
    show_cstr(b, "}");
    return;
  }
  case SD_K_BOX: {
    const sd_box *x = *(sd_box *const *)v;
    if (!x) show_cstr(b, "null");
    else show_value(b, x->t, sd_box_data(x), depth);
    return;
  }
  case SD_K_IFACE: {
    sd_iface x = *(const sd_iface *)v;
    if (!x.vt) show_cstr(b, "null");
    else show_value(b, x.obj->t, sd_box_data(x.obj), depth);
    return;
  }
  case SD_K_STRUCT:
    show_cstr(b, show_type_name(t));
    show_cstr(b, "{");
    show_fields(b, t->fields, t->nfields, t->field_names, v, depth);
    show_cstr(b, "}");
    return;
  case SD_K_ENUM: {
    int32_t tag = *(const int32_t *)v;
    show_cstr(b, show_type_name(t));
    if (tag < 0 || tag >= t->nvariants) return;
    const sd_variant *var = &t->variants[tag];
    show_cstr(b, ".");
    show_cstr(b, var->name);
    if (var->nfields == 0) return;
    show_cstr(b, "(");
    show_fields(b, var->fields, var->nfields, NULL, v, depth);
    show_cstr(b, ")");
    return;
  }
  case SD_K_CUSTOM: show_cstr(b, show_type_name(t)); return;
  }
}

static void show_into(show_buf *b, const sd_type *t, const void *v, size_t limit) {
  size_t start = b->n;
  b->limit = start + limit;
  b->cut = false;
  show_value(b, t, v, 0);
  if (b->cut) {
    b->cut = false;
    b->limit = b->n + 3;
    show_cstr(b, "...");
  }
  b->limit = SIZE_MAX;
}

sd_str sd_show(const sd_type *t, const void *v) {
  show_buf b = {NULL, 0, 0, SIZE_MAX, false};
  show_into(&b, t, v, SHOW_LIMIT);
  sd_str r = sd_str_new(b.p ? b.p : "", (int64_t)b.n);
  free(b.p);
  return r;
}

/* The value v of type t with its boxes and interfaces opened (or NULL for
 * null), and its type in *tp. */
static const void *show_open(const sd_type **tp, const void *v) {
  for (;;) {
    const sd_type *t = *tp;
    if (t->kind == SD_K_BOX) {
      const sd_box *x = *(sd_box *const *)v;
      if (!x) return NULL;
      *tp = x->t;
      v = sd_box_data(x);
    } else if (t->kind == SD_K_IFACE) {
      sd_iface x = *(const sd_iface *)v;
      if (!x.vt) return NULL;
      *tp = x.obj->t;
      v = sd_box_data(x.obj);
    } else {
      return v;
    }
  }
}

static void show_diff_values(show_buf *b, const sd_type *t, const void *x, const void *y) {
  show_into(b, t, x, SHOW_DIFF_LIMIT);
  show_cstr(b, " vs ");
  show_into(b, t, y, SHOW_DIFF_LIMIT);
}

/* Where the unequal a and b (of type t) first differ, for lists, maps and
 * structs (", first difference at index 2: 3 vs 4"); nothing otherwise. */
static void show_difference(show_buf *b, const sd_type *t, const void *a, const void *b2) {
  const sd_type *tb = t;
  a = show_open(&t, a);
  b2 = show_open(&tb, b2);
  if (!a || !b2 || t != tb) return;
  if (t->kind == SD_K_LIST) {
    const sd_list *x = *(sd_list *const *)a, *y = *(sd_list *const *)b2;
    if (!x || !y) return;
    int64_t n = x->len < y->len ? x->len : y->len;
    const char *dx = sd_list_data(x), *dy = sd_list_data(y);
    for (int64_t i = 0; i < n; i++) {
      const void *ex = dx + (size_t)i * x->et->size, *ey = dy + (size_t)i * y->et->size;
      if (sd_equal(x->et, ex, ey)) continue;
      char head[64];
      if (x->len != y->len) {
        snprintf(head, sizeof head, ", lengths %lld and %lld", (long long)x->len, (long long)y->len);
        show_cstr(b, head);
      }
      snprintf(head, sizeof head, ", first difference at index %lld: ", (long long)i);
      show_cstr(b, head);
      show_diff_values(b, x->et, ex, ey);
      return;
    }
    if (x->len != y->len) {
      char head[96];
      snprintf(head, sizeof head, ", lengths %lld and %lld, first extra element at index %lld: ", (long long)x->len, (long long)y->len, (long long)n);
      show_cstr(b, head);
      const sd_list *longer = x->len > y->len ? x : y;
      show_into(b, longer->et, (const char *)sd_list_data(longer) + (size_t)n * longer->et->size, SHOW_DIFF_LIMIT);
    }
    return;
  }
  if (t->kind == SD_K_MAP) {
    const sd_map *x = *(sd_map *const *)a, *y = *(sd_map *const *)b2;
    if (!x || !y) return;
    int64_t pos = 0;
    void *k, *xv;
    while (sd_map_next(x, &pos, &k, &xv)) {
      const void *yv = sd_map_get(y, k);
      if (yv && sd_equal(x->vt, xv, yv)) continue;
      show_cstr(b, ", first difference at key ");
      show_into(b, x->kt, k, SHOW_DIFF_LIMIT);
      show_cstr(b, ": ");
      if (yv) {
        show_diff_values(b, x->vt, xv, yv);
      } else {
        show_into(b, x->vt, xv, SHOW_DIFF_LIMIT);
        show_cstr(b, " vs missing");
      }
      return;
    }
    pos = 0;
    while (sd_map_next(y, &pos, &k, &xv)) {
      if (sd_map_get(x, k)) continue;
      show_cstr(b, ", first difference at key ");
      show_into(b, y->kt, k, SHOW_DIFF_LIMIT);
      show_cstr(b, ": missing vs ");
      show_into(b, y->vt, xv, SHOW_DIFF_LIMIT);
      return;
    }
    return;
  }
  if (t->kind == SD_K_STRUCT) {
    for (int32_t i = 0; i < t->nfields; i++) {
      const sd_field *f = &t->fields[i];
      const char *fx = (const char *)a + f->offset, *fy = (const char *)b2 + f->offset;
      if (sd_equal(f->type, fx, fy)) continue;
      show_cstr(b, ", first difference at ");
      if (t->field_names) {
        show_cstr(b, ".");
        show_cstr(b, t->field_names[i]);
      } else {
        char head[32];
        snprintf(head, sizeof head, "field %d", (int)i + 1);
        show_cstr(b, head);
      }
      show_cstr(b, ": ");
      show_diff_values(b, f->type, fx, fy);
      return;
    }
  }
}

sd_str sd_assert_show(const sd_type *t, const void *a, const void *b2, const char *op) {
  show_buf b = {NULL, 0, 0, SIZE_MAX, false};
  show_cstr(&b, "(");
  show_into(&b, t, a, SHOW_LIMIT);
  show_cstr(&b, " ");
  show_cstr(&b, op);
  show_cstr(&b, " ");
  show_into(&b, t, b2, SHOW_LIMIT);
  if (strcmp(op, "==") == 0 && !sd_equal(t, a, b2)) show_difference(&b, t, a, b2);
  show_cstr(&b, ")");
  sd_str r = sd_str_new(b.p, (int64_t)b.n);
  free(b.p);
  return r;
}

static const sd_test *test_list;
static int test_count;

static sd_err test_trampoline(void *p) {
  int64_t i = 0;
  sd_int_to_i64(*(const sd_int *)p, &i);
  return test_list[i].fn();
}

/* Runs every test in a goroutine of its own group, so that an error or a
 * crash fails only that test. Prints one line per failed test; writes
 * "<passed> <failed>\n" to the file named by args[1], if any. The
 * arguments after it are options: "-v" prints a line for every test that
 * passes too, "--run TEXT" runs only the tests whose description contains
 * TEXT, and "--dir DIR" runs the tests in the directory DIR (seed test
 * passes the package's directory, so that tests read their files by paths
 * relative to it, as Go's do; a relative results path is made absolute
 * before that, and the file is written only at the end, so that a test
 * program that dies leaves none). */
static sd_err test_run_all(sd_list *args) {
  int passed = 0, failed = 0;
  bool verbose = false;
  const char *filter = NULL;
  const char *dir = NULL;
  char *results = NULL;
  sd_task_set_name_("the test runner");
  for (int64_t a = 2; a < args->len; a++) {
    const char *opt = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(a)));
    if (strcmp(opt, "-v") == 0) verbose = true;
    else if (strcmp(opt, "--run") == 0 && a + 1 < args->len) filter = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(++a)));
    else if (strcmp(opt, "--dir") == 0 && a + 1 < args->len) dir = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(++a)));
  }
  if (args->len > 1) {
    const char *path = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(1)));
    char cwd[4096];
    if (dir && path[0] != '/' && getcwd(cwd, sizeof cwd)) results = sd_fmt("%s/%s", cwd, path);
    else results = sd_fmt("%s", path);
  }
  if (dir && chdir(dir) != 0) {
    fprintf(stderr, "seed test: cannot enter %s: %s\n", dir, strerror(errno));
    free(results);
    sd_exit_process(2);
  }
  for (int i = 0; i < test_count; i++) {
    const sd_test *t = &test_list[i];
    if (filter && !strstr(t->name, filter)) continue;
    sd_group *g = sd_group_begin();
    sd_int idx = SD_INT(i);
    /* A copy of int's descriptor named like a spawn's arguments, so that a
     * deadlock report names the test's goroutine (sd_spawn). */
    char *label = sd_fmt("spawn test \"%s\"", t->name);
    sd_type ty = sd_type_int;
    ty.name = label;
    sd_spawn(g, test_trampoline, &ty, &idx);
    sd_err e = sd_group_wait(g);
    free(label);
    if (sd_err_is_null(e)) {
      passed++;
      if (verbose) {
        fflush(stdout);
        printf("PASS \"%s\"\n", t->name);
        fflush(stdout);
      }
      continue;
    }
    failed++;
    sd_str m = sd_err_message(e);
    fflush(stdout);
    if (sd_err_is_assert(e)) {
      printf("FAIL \"%s\": %s\n", t->name, m->data);
    } else if (m->len >= 7 && memcmp(m->data, "crash: ", 7) == 0) {
      printf("FAIL \"%s\": %s:%d: %s\n", t->name, t->file, t->line, m->data);
      const char *trace = sd_err_trace(e);
      if (trace) fputs(trace, stdout);
    } else {
      printf("FAIL \"%s\": %s:%d: error: %s\n", t->name, t->file, t->line, m->data);
    }
    fflush(stdout);
    sd_str_release(m);
    sd_err_release(e);
  }
  if (results) {
    FILE *f = fopen(results, "w");
    if (f) {
      fprintf(f, "%d %d\n", passed, failed);
      fclose(f);
    }
    free(results);
  }
  fflush(stdout);
  sd_exit_process(failed > 0 ? 1 : 0);
}

int sd_test_main(int argc, char **argv, const sd_test *tests, int n) {
  test_list = tests;
  test_count = n;
  return sd_main(argc, argv, test_run_all);
}
