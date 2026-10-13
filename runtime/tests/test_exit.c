/* Whole-program behaviour: exit codes, crash/error messages, flushing,
 * deadlock detection. test_exit.sh runs each mode and checks the results. */
#include "seed.h"

#include <stdio.h>

static void defer_println(void *env) {
  sd_str s = *(sd_str *)env;
  sd_fmt_println(s);
  sd_str_release(s);
}

static void push_println_defer(const char *text) {
  sd_str s = sd_str_from_cstr(text);
  sd_defer_push(defer_println, &s, sizeof s);
}

typedef struct { sd_chan *ch; } A_ch;
static const sd_field F_ch[] = {SD_FIELD(A_ch, ch, &sd_type_chan)};
static const sd_type T_ch = SD_STRUCT_TYPE("ch", A_ch, F_ch);

static sd_err blocked_child(void *p) {
  A_ch *a = p;
  size_t mark = sd_defer_mark();
  push_println_defer("child cleanup");
  sd_int v;
  sd_chan_receive(a->ch, &v);
  sd_defer_run(mark);
  return SD_ERR_NULL;
}

static int recurse(int n) {
  volatile char buf[512];
  buf[0] = (char)n;
  return recurse(n + 1) + buf[0];
}

static sd_err overflow_child(void *p) {
  (void)p;
  return recurse(0) ? SD_ERR_NULL : SD_ERR_NULL;
}

static bool mode_is(sd_list *args, const char *m) {
  return args->len > 1 && strcmp(sd_str_cstr(SD_LIST_GET(args, sd_str, SD_INT(1))), m) == 0;
}

static sd_err seed_main(sd_list *args) {
  size_t mark = sd_defer_mark();
  if (mode_is(args, "crash")) {
    push_println_defer("deferred");
    sd_fmt_println(SD_STR_LIT("before"));
    sd_list *xs = sd_list_new(&sd_type_int, 0);
    sd_int v = SD_LIST_GET(xs, sd_int, SD_INT(5));
    (void)v;
  } else if (mode_is(args, "error")) {
    push_println_defer("main deferred");
    sd_fmt_println(SD_STR_LIT("main body"));
    sd_err e = sd_errors_new(SD_STR_LIT("shutting down"));
    sd_defer_run(mark);
    return e;
  } else if (mode_is(args, "exit")) {
    push_println_defer("not printed: os.exit skips defers");
    sd_fmt_print(SD_STR_LIT("buffered"));
    sd_os_exit(SD_INT(3));
  } else if (mode_is(args, "deadlock")) {
    sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(0));
    sd_fmt_println(SD_STR_LIT("waiting"));
    sd_int v;
    sd_chan_receive(c, &v);
  } else if (mode_is(args, "deadlock_group")) {
    sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(0));
    sd_group *g = sd_group_begin();
    A_ch a = {c};
    sd_spawn(g, blocked_child, &T_ch, &a);
    sd_spawn(g, blocked_child, &T_ch, &a);
    sd_err e = sd_group_wait(g);
    (void)e;
  } else if (mode_is(args, "group_crash_body")) {
    push_println_defer("main cleanup");
    sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(0));
    sd_group *g = sd_group_begin();
    A_ch a = {c};
    sd_spawn(g, blocked_child, &T_ch, &a);
    sd_int z = sd_int_div(SD_INT(1), SD_INT(0)); /* crash inside the group block */
    (void)z;
    sd_err e = sd_group_wait(g);
    (void)e;
  } else if (mode_is(args, "stack")) {
    push_println_defer("deferred");
    recurse(0);
  } else if (mode_is(args, "stack_goroutine")) {
    sd_group *g = sd_group_begin();
    sd_spawn(g, overflow_child, NULL, NULL);
    sd_err e = sd_group_wait(g);
    sd_str m = sd_err_message(e);
    sd_str parts[] = {SD_STR_LIT("group error: "), m};
    sd_str line = sd_str_concat_n(parts, 2);
    sd_fmt_println(line);
  } else if (mode_is(args, "eprint")) {
    sd_fmt_print(SD_STR_LIT("out1 "));
    sd_fmt_eprintln(SD_STR_LIT("err"));
    sd_fmt_println(SD_STR_LIT("out2"));
  } else if (mode_is(args, "timeout")) {
    sd_chan *never = sd_chan_new(&sd_type_int, SD_INT(0));
    sd_chan *t = sd_time_after(SD_INT(10000000));
    sd_int a, b;
    sd_select_case cs[2] = {{never, SD_RECV, &a, false}, {t, SD_RECV, &b, false}};
    if (sd_select(cs, 2) == 1) sd_fmt_println(SD_STR_LIT("timeout"));
    sd_time_sleep(SD_INT(1000000));
    sd_fmt_println(SD_STR_LIT("slept"));
  } else if (mode_is(args, "args")) {
    sd_fmt_println(sd_list_join(args, SD_STR_LIT(" ")));
  } else {
    return sd_errorf("unknown mode");
  }
  sd_defer_run(mark);
  return SD_ERR_NULL;
}

int main(int argc, char **argv) { return sd_main(argc, argv, seed_main); }
