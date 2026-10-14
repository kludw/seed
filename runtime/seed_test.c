/* seed test: the runner of test programs, and the values a failed assert
 * shows. */
#include "seed_internal.h"

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
 * passes too, and "--run TEXT" runs only the tests whose description
 * contains TEXT. */
static sd_err test_run_all(sd_list *args) {
  int passed = 0, failed = 0;
  bool verbose = false;
  const char *filter = NULL;
  sd_task_set_name_("the test runner");
  for (int64_t a = 2; a < args->len; a++) {
    const char *opt = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(a)));
    if (strcmp(opt, "-v") == 0) verbose = true;
    else if (strcmp(opt, "--run") == 0 && a + 1 < args->len) filter = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(++a)));
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
    } else {
      printf("FAIL \"%s\": %s:%d: error: %s\n", t->name, t->file, t->line, m->data);
    }
    fflush(stdout);
    sd_str_release(m);
    sd_err_release(e);
  }
  if (args->len > 1) {
    const char *path = sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(1)));
    FILE *f = fopen(path, "w");
    if (f) {
      fprintf(f, "%d %d\n", passed, failed);
      fclose(f);
    }
  }
  fflush(stdout);
  sd_exit_process(failed > 0 ? 1 : 0);
}

int sd_test_main(int argc, char **argv, const sd_test *tests, int n) {
  test_list = tests;
  test_count = n;
  return sd_main(argc, argv, test_run_all);
}
