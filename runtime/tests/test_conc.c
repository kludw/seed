/* Goroutines, groups, channels, select, timers, cancellation and defers.
 * Run under -fsanitize=thread as well as address,undefined. */
#include "check.h"

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/* Test-only shared state (Seed code can't have any; the tests use it to
 * observe deferred calls). */
static atomic_int defers_run;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static char event_log[1024];

static void log_event(const char *s) {
  pthread_mutex_lock(&log_mu);
  strncat(event_log, s, sizeof event_log - strlen(event_log) - 1);
  pthread_mutex_unlock(&log_mu);
}
static void log_reset(void) {
  pthread_mutex_lock(&log_mu);
  event_log[0] = 0;
  pthread_mutex_unlock(&log_mu);
}
static void log_get(char *out, size_t n) {
  pthread_mutex_lock(&log_mu);
  snprintf(out, n, "%s", event_log);
  pthread_mutex_unlock(&log_mu);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void check_err(sd_err e, const char *want, int line) {
  check_count++;
  if (!want) {
    if (!sd_err_is_null(e)) {
      sd_str m = sd_err_message(e);
      check_failures++;
      fprintf(stderr, "test_conc.c:%d: unexpected error: %s\n", line, sd_str_cstr(m));
      sd_str_release(m);
    }
  } else if (sd_err_is_null(e)) {
    check_failures++;
    fprintf(stderr, "test_conc.c:%d: expected error \"%s\", got null\n", line, want);
  } else {
    sd_str m = sd_err_message(e);
    if (!strstr(sd_str_cstr(m), want)) {
      check_failures++;
      fprintf(stderr, "test_conc.c:%d: error \"%s\" lacks \"%s\"\n", line, sd_str_cstr(m), want);
    }
    sd_str_release(m);
  }
  sd_err_release(e);
}
#define CHECK_ERR(e, want) check_err((e), (want), __LINE__)

/* ---- argument structs for goroutines (what a code generator emits) ---- */

typedef struct { sd_chan *ch; sd_int n; } A_chan_n;
static const sd_field F_chan_n[] = {SD_FIELD(A_chan_n, ch, &sd_type_chan), SD_FIELD(A_chan_n, n, &sd_type_int)};
static const sd_type T_chan_n = SD_STRUCT_TYPE("chan_n", A_chan_n, F_chan_n);

typedef struct { sd_chan *a, *b; sd_int n; } A_chan2_n;
static const sd_field F_chan2_n[] = {SD_FIELD(A_chan2_n, a, &sd_type_chan), SD_FIELD(A_chan2_n, b, &sd_type_chan),
                                     SD_FIELD(A_chan2_n, n, &sd_type_int)};
static const sd_type T_chan2_n = SD_STRUCT_TYPE("chan2_n", A_chan2_n, F_chan2_n);

typedef struct { sd_chan *out; sd_list *xs; } A_chan_list;
static const sd_field F_chan_list[] = {SD_FIELD(A_chan_list, out, &sd_type_chan),
                                       SD_FIELD(A_chan_list, xs, &sd_type_list)};
static const sd_type T_chan_list = SD_STRUCT_TYPE("chan_list", A_chan_list, F_chan_list);

/* ---- basic channel behaviour (single goroutine) ---- */

static void test_buffered_basic(void) {
  sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(3));
  for (int i = 1; i <= 3; i++) sd_chan_send(c, (sd_int[]){SD_INT(i)});
  sd_chan_close(c);
  sd_int v;
  for (int i = 1; i <= 3; i++) {
    CHECK(sd_chan_receive(c, &v));
    CHECK(sd_int_eq(v, SD_INT(i)));
  }
  CHECK(!sd_chan_receive(c, &v)); /* closed and empty: null */
  CHECK(!sd_chan_receive(c, &v));
  sd_chan_release(c);
}

static sd_err send_closed_body(void *p) {
  sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(1));
  sd_chan_close(c);
  sd_chan_send(c, (sd_int[]){SD_INT(1)}); /* crashes; c leaks (crash) */
  return SD_ERR_NULL;
}

static sd_err close_twice_body(void *p) {
  sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(0));
  sd_chan_close(c);
  sd_chan_close(c);
  return SD_ERR_NULL;
}

static void test_channel_crashes(void) {
  CHECK_ERR(sd_catch(send_closed_body, NULL), "crash: send on closed channel");
  CHECK_ERR(sd_catch(close_twice_body, NULL), "crash: close of closed channel");
}

/* ---- copy semantics: tests/full/chan_copy_semantics.sd ---- */

static sd_err copy_sender(void *p) {
  A_chan_n *a = p;
  sd_list *xs = sd_list_new(&sd_type_int, 0);
  SD_LIST_PUSH(&xs, sd_int, SD_INT(1));
  SD_LIST_PUSH(&xs, sd_int, SD_INT(2));
  sd_chan_send(a->ch, &xs);
  SD_LIST_PUSH(&xs, sd_int, SD_INT(3));
  sd_chan_send(a->ch, &xs);
  *(sd_int *)sd_list_at_mut(&xs, SD_INT(0)) = SD_INT(100);
  sd_list_release(xs);
  return SD_ERR_NULL;
}

static void test_copy_semantics(void) {
  sd_chan *ch = sd_chan_new(&sd_type_list, SD_INT(2));
  sd_group *g = sd_group_begin();
  A_chan_n args = {ch, SD_INT(0)};
  sd_spawn(g, copy_sender, &T_chan_n, &args);
  CHECK_ERR(sd_group_wait(g), NULL);
  sd_list *first = NULL, *second = NULL;
  CHECK(sd_chan_receive(ch, &first) && sd_chan_receive(ch, &second));
  CHECK(first->len == 2 && second->len == 3 && first->rc == 1 && second->rc == 1);
  CHECK(sd_int_eq(SD_LIST_GET(first, sd_int, SD_INT(0)), SD_INT(1)));
  sd_list_release(first);
  sd_list_release(second);
  sd_chan_release(ch);
}

/* ---- ping-pong over unbuffered channels ---- */

static sd_err pinger(void *p) {
  A_chan2_n *a = p;
  int64_t n = sd_int_small_value(a->n);
  for (int64_t i = 0; i < n; i++) {
    sd_chan_send(a->a, (sd_int[]){SD_INT(i)});
    sd_int v;
    if (!sd_chan_receive(a->b, &v)) return sd_errorf("pong closed");
    if (!sd_int_eq(v, SD_INT(i + 1))) return sd_errorf("bad pong");
  }
  sd_chan_close(a->a);
  return SD_ERR_NULL;
}

static sd_err ponger(void *p) {
  A_chan2_n *a = p;
  sd_int v;
  while (sd_chan_receive(a->a, &v)) {
    sd_int w = sd_int_add(v, SD_INT(1));
    sd_chan_send(a->b, &w);
  }
  return SD_ERR_NULL;
}

static void test_ping_pong(int64_t n) {
  sd_chan *a = sd_chan_new(&sd_type_int, SD_INT(0)), *b = sd_chan_new(&sd_type_int, SD_INT(0));
  A_chan2_n args = {a, b, SD_INT(n)};
  double t0 = now_s();
  sd_group *g = sd_group_begin();
  sd_spawn(g, pinger, &T_chan2_n, &args);
  sd_spawn(g, ponger, &T_chan2_n, &args);
  CHECK_ERR(sd_group_wait(g), NULL);
  double dt = now_s() - t0;
  printf("ping-pong: %lld round trips in %.3fs (%.2f us per round trip)\n", (long long)n, dt, dt / (double)n * 1e6);
  sd_chan_release(a);
  sd_chan_release(b);
}

/* ---- 1000 goroutines in a group, with deep-copied list arguments ---- */

static sd_err summer(void *p) {
  A_chan_list *a = p;
  if (a->xs->rc != 1) return sd_errorf("argument list is shared");
  sd_int sum = SD_INT(0);
  for (int64_t i = 0; i < a->xs->len; i++) {
    sd_int s = sd_int_add(sum, SD_LIST_GET(a->xs, sd_int, SD_INT(i)));
    sd_int_release(sum);
    sum = s;
  }
  SD_LIST_PUSH(&a->xs, sd_int, SD_INT(-1)); /* its own copy: mutating is fine */
  sd_chan_send(a->out, &sum);
  sd_int_release(sum);
  return SD_ERR_NULL;
}

static void test_thousand(void) {
  const int N = 1000;
  sd_chan *out = sd_chan_new(&sd_type_int, SD_INT(N));
  sd_list *xs = sd_list_new(&sd_type_int, 0);
  for (int i = 1; i <= 100; i++) SD_LIST_PUSH(&xs, sd_int, SD_INT(i));
  double t0 = now_s();
  sd_group *g = sd_group_begin();
  for (int i = 0; i < N; i++) {
    A_chan_list args = {out, xs};
    sd_spawn(g, summer, &T_chan_list, &args);
  }
  CHECK_ERR(sd_group_wait(g), NULL);
  double dt = now_s() - t0;
  sd_chan_close(out);
  int64_t count = 0, total = 0;
  sd_int v;
  while (sd_chan_receive(out, &v)) {
    count++;
    total += sd_int_small_value(v);
  }
  CHECK(count == N && total == (int64_t)N * 5050);
  CHECK(xs->rc == 1 && xs->len == 100);
  printf("group: %d goroutines started and joined in %.3fs\n", N, dt);
  sd_list_release(xs);
  sd_chan_release(out);
}

/* ---- errors, crashes and cancellation ---- */

static void defer_count(void *env) {
  (void)env;
  atomic_fetch_add(&defers_run, 1);
}
static void defer_log(void *env) { log_event(*(const char **)env); }

/* waiter(never, ready): announces itself on ready, then blocks forever. */
static sd_err waiter(void *p) {
  A_chan2_n *a = p;
  size_t mark = sd_defer_mark();
  sd_defer_push(defer_count, NULL, 0);
  const char *m1 = "a", *m2 = "b";
  sd_defer_push(defer_log, &m1, sizeof m1);
  sd_defer_push(defer_log, &m2, sizeof m2);
  sd_chan_send(a->b, (sd_int[]){SD_INT(1)});
  sd_int v;
  if (sd_chan_receive(a->a, &v)) { /* never receives: stopped here */
    sd_defer_run(mark);
    return sd_errorf("unexpected value");
  }
  sd_defer_run(mark);
  return SD_ERR_NULL;
}

/* failing(_, ready, n): waits until n goroutines are ready, then fails. */
static sd_err failing(void *p) {
  A_chan2_n *a = p;
  for (int64_t i = 0; i < sd_int_small_value(a->n); i++) {
    sd_int v;
    sd_chan_receive(a->b, &v);
  }
  return sd_errorf("worker failed after %lld", (long long)sd_int_small_value(a->n));
}

static sd_err crasher(void *p) {
  size_t mark = sd_defer_mark();
  sd_defer_push(defer_count, NULL, 0);
  sd_list *xs = sd_list_new(&sd_type_int, 0);
  sd_int v = SD_LIST_GET(xs, sd_int, SD_INT(10));
  sd_defer_run(mark);
  return sd_int_is_null(v) ? SD_ERR_NULL : SD_ERR_NULL;
}

static void test_group_error(void) {
  sd_chan *never = sd_chan_new(&sd_type_int, SD_INT(0)), *ready = sd_chan_new(&sd_type_int, SD_INT(100));
  atomic_store(&defers_run, 0);
  log_reset();
  sd_group *g = sd_group_begin();
  A_chan2_n w = {never, ready, SD_INT(0)}, f = {never, ready, SD_INT(2)};
  sd_spawn(g, waiter, &T_chan2_n, &w);
  sd_spawn(g, failing, &T_chan2_n, &f);
  sd_spawn(g, waiter, &T_chan2_n, &w);
  CHECK_ERR(sd_group_wait(g), "worker failed after 2");
  CHECK(atomic_load(&defers_run) == 2);
  char log[64];
  log_get(log, sizeof log);
  /* each waiter runs "b" then "a" (newest first); the two may interleave */
  CHECK(strlen(log) == 4 && log[0] == 'b' && log[3] == 'a');

  /* a crash becomes the group's error; the crasher's defers run */
  atomic_store(&defers_run, 0);
  g = sd_group_begin();
  sd_spawn(g, waiter, &T_chan2_n, &w);
  sd_spawn(g, crasher, NULL, NULL);
  CHECK_ERR(sd_group_wait(g), "crash: index out of range: 10 with length 0");
  CHECK(atomic_load(&defers_run) == 2);
  sd_chan_release(never);
  sd_chan_release(ready);
}

/* nested groups: cancelling a goroutine that waits in its own group cancels
 * that group too */
static sd_err inner_parent(void *p) {
  A_chan2_n *a = p;
  size_t mark = sd_defer_mark();
  sd_defer_push(defer_count, NULL, 0);
  sd_group *g = sd_group_begin();
  for (int i = 0; i < 5; i++) sd_spawn(g, waiter, &T_chan2_n, a);
  sd_err e = sd_group_wait(g);
  sd_err_release(e);
  sd_int v;
  sd_chan_receive(a->a, &v); /* stopped here (cancelled) */
  sd_defer_run(mark);
  return SD_ERR_NULL;
}

static void test_nested_cancel(void) {
  sd_chan *never = sd_chan_new(&sd_type_int, SD_INT(0)), *ready = sd_chan_new(&sd_type_int, SD_INT(100));
  atomic_store(&defers_run, 0);
  sd_group *g = sd_group_begin();
  A_chan2_n w = {never, ready, SD_INT(0)}, f = {never, ready, SD_INT(5)};
  sd_spawn(g, inner_parent, &T_chan2_n, &w);
  sd_spawn(g, failing, &T_chan2_n, &f);
  CHECK_ERR(sd_group_wait(g), "worker failed after 5");
  CHECK(atomic_load(&defers_run) == 6); /* 5 inner waiters + inner_parent */
  sd_chan_release(never);
  sd_chan_release(ready);
}

/* defers during a crash: newest first, a crash inside a deferred call does
 * not stop the others, and the first message wins */
static void defer_crash(void *env) {
  (void)env;
  log_event("X");
  sd_crash("second crash");
}

static sd_err crash_with_defers(void *p) {
  const char *m1 = "1", *m2 = "2";
  sd_defer_push(defer_log, &m1, sizeof m1);
  sd_defer_push(defer_crash, NULL, 0);
  sd_defer_push(defer_log, &m2, sizeof m2);
  sd_crash("first crash");
}

static void test_defer_crash(void) {
  log_reset();
  sd_group *g = sd_group_begin();
  sd_spawn(g, crash_with_defers, NULL, NULL);
  CHECK_ERR(sd_group_wait(g), "crash: first crash");
  char log[64];
  log_get(log, sizeof log);
  CHECK_STREQ(log, "2X1");
}

/* ---- select ---- */

static void test_select_fairness(void) {
  const int N = 100000;
  sd_chan *a = sd_chan_new(&sd_type_int, SD_INT(N)), *b = sd_chan_new(&sd_type_int, SD_INT(N));
  for (int i = 0; i < N; i++) {
    sd_chan_send(a, (sd_int[]){SD_INT(1)});
    sd_chan_send(b, (sd_int[]){SD_INT(2)});
  }
  int counts[2] = {0, 0};
  double t0 = now_s();
  for (int i = 0; i < N; i++) {
    sd_int x, y;
    sd_select_case cs[2] = {{a, SD_RECV, &x, false}, {b, SD_RECV, &y, false}};
    int k = sd_select(cs, 2);
    CHECK(cs[k].ok);
    counts[k]++;
  }
  double dt = now_s() - t0;
  printf("select fairness: %d / %d (%.0f ns per select)\n", counts[0], counts[1], dt / N * 1e9);
  CHECK(counts[0] > N * 45 / 100 && counts[1] > N * 45 / 100);
  sd_chan_release(a);
  sd_chan_release(b);
}

static void test_select_cases(void) {
  /* tests/full/select_send.sd */
  sd_chan *full = sd_chan_new(&sd_type_int, SD_INT(1)), *empty = sd_chan_new(&sd_type_int, SD_INT(1));
  sd_chan_send(full, (sd_int[]){SD_INT(1)});
  sd_int two = SD_INT(2), three = SD_INT(3);
  sd_select_case cs[2] = {{full, SD_SEND, &two, false}, {empty, SD_SEND, &three, false}};
  CHECK(sd_select(cs, 2) == 1);
  sd_chan *idle = sd_chan_new(&sd_type_int, SD_INT(0)), *ready = sd_chan_new(&sd_type_int, SD_INT(1));
  sd_chan_send(ready, (sd_int[]){SD_INT(5)});
  sd_int nine = SD_INT(9), got;
  sd_select_case cs2[2] = {{idle, SD_SEND, &nine, false}, {ready, SD_RECV, &got, false}};
  CHECK(sd_select(cs2, 2) == 1 && cs2[1].ok && sd_int_eq(got, SD_INT(5)));
  /* a closed channel is always ready, with ok == false */
  sd_chan_close(idle);
  sd_select_case cs3[2] = {{idle, SD_RECV, &got, true}, {ready, SD_RECV, &got, true}};
  CHECK(sd_select(cs3, 2) == 0 && !cs3[0].ok);
  /* timeout */
  sd_chan *never = sd_chan_new(&sd_type_int, SD_INT(0));
  double t0 = now_s();
  sd_chan *timer = sd_time_after(SD_INT(20000000)); /* 20 ms */
  sd_int tv;
  sd_select_case cs4[2] = {{never, SD_RECV, &got, false}, {timer, SD_RECV, &tv, false}};
  CHECK(sd_select(cs4, 2) == 1 && cs4[1].ok);
  double dt = now_s() - t0;
  CHECK(dt >= 0.019 && dt < 2.0);
  sd_chan_release(timer);
  sd_chan *chans[] = {full, empty, idle, ready, never};
  for (int i = 0; i < 5; i++) sd_chan_release(chans[i]);
}

/* select across goroutines: tests/full/select_goroutines.sd */
static sd_err produce(void *p) {
  A_chan_n *a = p;
  int64_t start = sd_int_small_value(a->n);
  for (int64_t i = 0; i < 1000; i++) sd_chan_send(a->ch, (sd_int[]){SD_INT(start + i)});
  sd_chan_close(a->ch);
  return SD_ERR_NULL;
}

static sd_err merge(void *p) {
  A_chan2_n *a = p;
  bool aopen = true, bopen = true;
  int64_t sum = 0, count = 0;
  while (aopen || bopen) {
    sd_int x;
    sd_select_case cs[2];
    int n = 0;
    if (aopen) cs[n++] = (sd_select_case){a->a, SD_RECV, &x, false};
    if (bopen) cs[n++] = (sd_select_case){a->b, SD_RECV, &x, false};
    int k = sd_select(cs, n);
    if (!cs[k].ok) {
      if (cs[k].ch == a->a) aopen = false;
      else bopen = false;
      continue;
    }
    sum += sd_int_small_value(x);
    count++;
  }
  if (count != 2000 || sum != (999 * 1000 / 2) + (1000000 * 1000 + 999 * 1000 / 2)) return sd_errorf("bad merge");
  return SD_ERR_NULL;
}

static void test_select_goroutines(void) {
  sd_chan *a = sd_chan_new(&sd_type_int, SD_INT(0)), *b = sd_chan_new(&sd_type_int, SD_INT(0));
  sd_group *g = sd_group_begin();
  A_chan_n pa = {a, SD_INT(0)}, pb = {b, SD_INT(1000000)};
  A_chan2_n m = {a, b, SD_INT(0)};
  sd_spawn(g, produce, &T_chan_n, &pa);
  sd_spawn(g, produce, &T_chan_n, &pb);
  sd_spawn(g, merge, &T_chan2_n, &m);
  CHECK_ERR(sd_group_wait(g), NULL);
  sd_chan_release(a);
  sd_chan_release(b);
}

/* A goroutine blocked in a select (with a send case holding a value) is
 * cancelled; its defers run. */
static sd_err select_blocker(void *p) {
  A_chan2_n *a = p; /* a: never (int), b: ready (int); sends a list on a list channel */
  sd_defer_push(defer_count, NULL, 0);
  sd_chan *lists = sd_chan_new(&sd_type_list, SD_INT(0));
  sd_list *payload = sd_str_split(SD_STR_LIT("a b c"), SD_STR_LIT(" "));
  sd_chan_send(a->b, (sd_int[]){SD_INT(1)});
  sd_int x;
  sd_select_case cs[2] = {{a->a, SD_RECV, &x, false}, {lists, SD_SEND, &payload, false}};
  sd_select(cs, 2);
  return sd_errorf("select returned");
}

static void test_select_cancel(void) {
  atomic_store(&defers_run, 0);
  sd_chan *never = sd_chan_new(&sd_type_int, SD_INT(0)), *ready = sd_chan_new(&sd_type_int, SD_INT(100));
  A_chan2_n args = {never, ready, SD_INT(0)}, fargs = {never, ready, SD_INT(4)};
  for (int round = 0; round < 50; round++) {
    sd_group *g = sd_group_begin();
    for (int i = 0; i < 4; i++) sd_spawn(g, select_blocker, &T_chan2_n, &args);
    sd_spawn(g, failing, &T_chan2_n, &fargs);
    CHECK_ERR(sd_group_wait(g), "worker failed after 4");
  }
  CHECK(atomic_load(&defers_run) == 200);
  sd_chan_release(never);
  sd_chan_release(ready);
}

/* A pipeline of goroutines connected by unbuffered channels. */
typedef struct { sd_chan *in, *out; } A_stage;
static const sd_field F_stage[] = {SD_FIELD(A_stage, in, &sd_type_chan), SD_FIELD(A_stage, out, &sd_type_chan)};
static const sd_type T_stage = SD_STRUCT_TYPE("stage", A_stage, F_stage);

static sd_err stage(void *p) {
  A_stage *a = p;
  sd_str s;
  while (sd_chan_receive(a->in, &s)) {
    sd_str t = sd_str_concat(s, SD_STR_LIT("."));
    sd_chan_send(a->out, &t);
    sd_str_release(s);
    sd_str_release(t);
  }
  sd_chan_close(a->out);
  return SD_ERR_NULL;
}

static sd_err feeder(void *p) {
  A_stage *a = p;
  for (int i = 0; i < 100; i++) {
    sd_str s = sd_int_to_str(SD_INT(i));
    sd_chan_send(a->out, &s);
    sd_str_release(s);
  }
  sd_chan_close(a->out);
  return SD_ERR_NULL;
}

static void test_pipeline(void) {
  enum { STAGES = 50 };
  sd_chan *cs[STAGES + 1];
  for (int i = 0; i <= STAGES; i++) cs[i] = sd_chan_new(&sd_type_str, SD_INT(i == STAGES ? 100 : 0));
  sd_group *g = sd_group_begin();
  A_stage f = {NULL, cs[0]};
  sd_spawn(g, feeder, &T_stage, &f);
  for (int i = 0; i < STAGES; i++) {
    A_stage s = {cs[i], cs[i + 1]};
    sd_spawn(g, stage, &T_stage, &s);
  }
  CHECK_ERR(sd_group_wait(g), NULL);
  sd_str s;
  int n = 0;
  while (sd_chan_receive(cs[STAGES], &s)) {
    CHECK(s->len == (n < 10 ? 1 : 2) + STAGES);
    sd_str_release(s);
    n++;
  }
  CHECK(n == 100);
  for (int i = 0; i <= STAGES; i++) sd_chan_release(cs[i]);
}

/* maps through channels: tests/full/parallel_word_count.sd */
typedef struct { sd_str chunk; sd_chan *out; } A_count;
static const sd_field F_count[] = {SD_FIELD(A_count, chunk, &sd_type_str), SD_FIELD(A_count, out, &sd_type_chan)};
static const sd_type T_count = SD_STRUCT_TYPE("count", A_count, F_count);

static sd_err count_chunk(void *p) {
  A_count *a = p;
  sd_map *counts = sd_map_new(&sd_type_str, &sd_type_int);
  sd_list *words = sd_str_split(a->chunk, SD_STR_LIT(" "));
  for (int64_t i = 0; i < words->len; i++) {
    sd_str w = SD_LIST_GET(words, sd_str, SD_INT(i));
    sd_int *c = sd_map_get(counts, &w);
    sd_int v = c ? sd_int_add(*c, SD_INT(1)) : SD_INT(1);
    sd_str k = sd_str_retain(w);
    sd_map_set(&counts, &k, &v);
  }
  sd_chan_send(a->out, &counts);
  sd_map_release(counts);
  sd_list_release(words);
  return SD_ERR_NULL;
}

static void test_word_count(void) {
  const char *chunks[] = {"the quick brown fox", "jumps over the lazy dog", "the dog sleeps"};
  sd_chan *partials = sd_chan_new(&sd_type_map, SD_INT(3));
  sd_group *g = sd_group_begin();
  for (int i = 0; i < 3; i++) {
    A_count a = {sd_str_from_cstr(chunks[i]), partials};
    sd_spawn(g, count_chunk, &T_count, &a);
    sd_str_release(a.chunk);
  }
  CHECK_ERR(sd_group_wait(g), NULL);
  sd_chan_close(partials);
  sd_map *total = sd_map_new(&sd_type_str, &sd_type_int);
  sd_map *part;
  while (sd_chan_receive(partials, &part)) {
    int64_t pos = 0;
    void *k, *v;
    while (sd_map_next(part, &pos, &k, &v)) {
      sd_int *have = sd_map_get(total, k);
      sd_int nv = have ? sd_int_add(*have, *(sd_int *)v) : *(sd_int *)v;
      sd_str key = sd_str_retain(*(sd_str *)k);
      sd_map_set(&total, &key, &nv);
    }
    sd_map_release(part);
  }
  sd_list *words = sd_map_keys(total);
  sd_list_sort(&words);
  CHECK_SDSTR(sd_list_join(words, SD_STR_LIT(" ")), "brown dog fox jumps lazy over quick sleeps the");
  sd_str the = SD_STR_LIT("the");
  CHECK(sd_int_eq(*(sd_int *)sd_map_get(total, &the), SD_INT(3)));
  sd_list_release(words);
  sd_map_release(total);
  sd_chan_release(partials);
}

int main(int argc, char **argv) {
  int64_t live0 = sd_live_objects();
  int64_t pp = argc > 1 ? atol(argv[1]) : 20000;
  test_buffered_basic();
  test_copy_semantics();
  test_ping_pong(pp);
  test_thousand();
  test_group_error();
  test_nested_cancel();
  test_defer_crash();
  test_select_fairness();
  test_select_cases();
  test_select_goroutines();
  test_select_cancel();
  test_pipeline();
  test_word_count();
  int64_t live1 = sd_live_objects();
  /* Crashed and cancelled goroutines leak the objects their frames held
   * (unwinding is a longjmp): 2 per cancelled select_blocker, 1 for crasher. */
  if (live0 >= 0) CHECK(live1 - live0 == 200 * 2 + 1);
  test_channel_crashes();
  return check_done("test_conc");
}
