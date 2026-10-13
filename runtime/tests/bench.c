/* Rough performance numbers for runtime/README.md. Build with -O2, no
 * sanitizers: run.sh bench */
#include "seed.h"

#include <stdio.h>
#include <time.h>

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static volatile int64_t sink;

__attribute__((noinline)) static sd_int add_loop(sd_int n) {
  sd_int sum = SD_INT(0);
  for (sd_int i = SD_INT(0); sd_int_lt(i, n);) {
    sd_int t = sd_int_add(sum, i);
    sd_int_release(sum);
    sum = t;
    sd_int u = sd_int_add(i, SD_INT(1));
    sd_int_release(i);
    i = u;
  }
  return sum;
}

typedef struct { sd_chan *in, *out; sd_int n; } A_pp;
static const sd_field F_pp[] = {SD_FIELD(A_pp, in, &sd_type_chan), SD_FIELD(A_pp, out, &sd_type_chan),
                                SD_FIELD(A_pp, n, &sd_type_int)};
static const sd_type T_pp = SD_STRUCT_TYPE("pp", A_pp, F_pp);

static sd_err producer(void *p) {
  A_pp *a = p;
  for (int64_t i = 0; i < sd_int_small_value(a->n); i++) sd_chan_send(a->out, (sd_int[]){SD_INT(i)});
  sd_chan_close(a->out);
  return SD_ERR_NULL;
}
static sd_err consumer(void *p) {
  A_pp *a = p;
  sd_int v;
  int64_t s = 0;
  while (sd_chan_receive(a->in, &v)) s += sd_int_small_value(v);
  sink = s;
  return SD_ERR_NULL;
}
static sd_err nothing(void *p) {
  (void)p;
  return SD_ERR_NULL;
}

static sd_err bench_main(sd_list *args) {
  (void)args;
  const int64_t N = 200000000;
  double t0 = now_s();
  sd_int r = add_loop(SD_INT(N));
  double t1 = now_s();
  sink = sd_int_small_value(r);
  printf("int loop (small ints): %.2f ns per iteration of sum = sum + i; i = i + 1; i < n (with releases)\n",
         (t1 - t0) / N * 1e9);
  double t2;

  sd_int big = sd_int_from_lit("123456789012345678901234567890");
  t0 = now_s();
  for (int i = 0; i < 1000000; i++) {
    sd_int x = sd_int_mul(big, big);
    sd_int_release(x);
  }
  printf("bigint 30-digit multiply: %.0f ns\n", (now_s() - t0) / 1e6 * 1e9);
  sd_int_release(big);

  const int64_t M = 50000000;
  t0 = now_s();
  sd_list *l = sd_list_new(&sd_type_int, 0);
  for (int64_t i = 0; i < M; i++) SD_LIST_PUSH(&l, sd_int, SD_INT(i));
  t1 = now_s();
  sd_list_release(l);
  printf("list push (SD_LIST_PUSH, []int): %.0f M pushes/s (%.2f ns each)\n", M / (t1 - t0) / 1e6, (t1 - t0) / M * 1e9);
  t0 = now_s();
  l = sd_list_new(&sd_type_int, 0);
  for (int64_t i = 0; i < M; i++) {
    sd_int v = SD_INT(i);
    sd_list_push(&l, &v);
  }
  t1 = now_s();
  sd_list_release(l);
  printf("list push (sd_list_push, []int): %.0f M pushes/s (%.2f ns each)\n", M / (t1 - t0) / 1e6, (t1 - t0) / M * 1e9);

  t0 = now_s();
  sd_list *ss = sd_list_new(&sd_type_str, 0);
  for (int64_t i = 0; i < 1000000; i++) SD_LIST_PUSH(&ss, sd_str, sd_int_to_str(SD_INT(i)));
  sd_str joined = sd_list_join(ss, SD_STR_LIT(","));
  t1 = now_s();
  printf("1M int.string() + push + join: %.0f ms (%zu bytes)\n", (t1 - t0) * 1e3, (size_t)joined->len);
  sd_str_release(joined);

  t0 = now_s();
  sd_map *m = sd_map_new(&sd_type_str, &sd_type_int);
  for (int64_t i = 0; i < ss->len; i++) {
    sd_str k = sd_str_retain(SD_LIST_GET(ss, sd_str, SD_INT(i)));
    sd_int v = SD_INT(i);
    sd_map_set(&m, &k, &v);
  }
  t1 = now_s();
  int64_t hits = 0;
  for (int64_t i = 0; i < ss->len; i++) hits += sd_map_get(m, &SD_LIST_GET(ss, sd_str, SD_INT(i))) != NULL;
  t2 = now_s();
  sink = hits;
  printf("map[string]int: %.0f ns per insert, %.0f ns per lookup (1M keys)\n", (t1 - t0) / 1e6 * 1e9,
         (t2 - t1) / 1e6 * 1e9);
  sd_map_release(m);
  sd_list_release(ss);

  t0 = now_s();
  sd_str acc = sd_str_empty();
  for (int i = 0; i < 10000000; i++) acc = sd_str_append(acc, SD_STR_LIT("x"));
  t1 = now_s();
  printf("string append in place: %.2f ns per append\n", (t1 - t0) / 1e7 * 1e9);
  sd_str_release(acc);

  const int64_t C = 1000000;
  sd_chan *bc = sd_chan_new(&sd_type_int, SD_INT(1024));
  t0 = now_s();
  for (int64_t i = 0; i < C; i++) {
    sd_chan_send(bc, (sd_int[]){SD_INT(i)});
    sd_int v;
    sd_chan_receive(bc, &v);
  }
  t1 = now_s();
  printf("buffered channel send+receive, same goroutine: %.0f ns\n", (t1 - t0) / C * 1e9);
  sd_chan_release(bc);

  int caps[] = {0, 1024};
  for (int ci = 0; ci < 2; ci++) {
    sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(caps[ci]));
    A_pp a = {c, c, SD_INT(C)};
    t0 = now_s();
    sd_group *g = sd_group_begin();
    sd_spawn(g, producer, &T_pp, &a);
    sd_spawn(g, consumer, &T_pp, &a);
    sd_err e = sd_group_wait(g);
    sd_err_release(e);
    t1 = now_s();
    printf("channel(%d) producer -> consumer goroutines: %.0f ns per value\n", caps[ci], (t1 - t0) / C * 1e9);
    sd_chan_release(c);
  }

  t0 = now_s();
  for (int round = 0; round < 10; round++) {
    sd_group *g = sd_group_begin();
    for (int i = 0; i < 100; i++) sd_spawn(g, nothing, NULL, NULL);
    sd_err e = sd_group_wait(g);
    sd_err_release(e);
  }
  printf("spawn + join: %.1f us per goroutine\n", (now_s() - t0) / 1000 * 1e6);
  return SD_ERR_NULL;
}

int main(int argc, char **argv) { return sd_main(argc, argv, bench_main); }
