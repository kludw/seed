/* A hand-written "generated" program: what the Seed compiler should emit
 * for the program below, following runtime/README.md. Compiled and run by
 * runtime/tests/run.sh; its output must equal example.out.
 *
 *   package main
 *
 *   import "errors"
 *   import "fmt"
 *
 *   type Item struct {
 *       string name
 *       []int counts
 *   }
 *
 *   fn total(Item it) int {
 *       int sum = 0
 *       for (int i = 0; i < it.counts.len(); i++) {
 *           sum = sum + it.counts[i]
 *       }
 *       return sum
 *   }
 *
 *   fn (Item it) mut add(int n) {
 *       it.counts.push(n)
 *   }
 *
 *   fn worker(int id, []Item items, chan string out) error {
 *       defer fmt.eprintln("worker " + id.string() + " done")
 *       for (int i = 0; i < items.len(); i++) {
 *           out.send(id.string() + ":" + items[i].name + "=" + total(items[i]).string())
 *       }
 *       return null
 *   }
 *
 *   fn check(int n) error {
 *       defer fmt.println("check done")
 *       if (n > 100) {
 *           return errors.new("too big: " + n.string())
 *       }
 *       return null
 *   }
 *
 *   fn main([]string args) error {
 *       defer fmt.println("main: deferred")
 *       []Item items = []Item{Item{name: "a", counts: []int{1, 2}}, Item{name: "b", counts: []int{}}}
 *       []Item other = items
 *       other[1].add(40)
 *       other[1].add(1267650600228229401496703205376)
 *       fmt.println("items[1] has " + items[1].counts.len().string() + ", other[1] has " + other[1].counts.len().string())
 *       chan string out = chan string(10)
 *       error err = group {
 *           spawn worker(1, items, out)
 *           spawn worker(2, other, out)
 *       }
 *       if (err != null) {
 *           return err
 *       }
 *       out.close()
 *       []string lines = []string{}
 *       for (;;) {
 *           string? s = out.receive()
 *           if (s == null) {
 *               break
 *           }
 *           lines.push(s)
 *       }
 *       lines.sort()
 *       fmt.println(lines.join(" "))
 *       map[string]int totals = map[string]int{}
 *       for (int i = 0; i < other.len(); i++) {
 *           totals[other[i].name] = total(other[i])
 *       }
 *       int? b = totals["b"]
 *       if (b != null) {
 *           fmt.println("b total: " + b.string())
 *       }
 *       error e = check(total(other[1]))
 *       if (e != null) {
 *           fmt.println("check failed: " + e.message())
 *       }
 *       return null
 *   }
 */
#include "seed.h"

/* ---- types ---- */

typedef struct S_Item {
  sd_str f_name;
  sd_list *f_counts; /* []int */
} S_Item;
static const sd_field F_Item[] = {
  SD_FIELD(S_Item, f_name, &sd_type_str),
  SD_FIELD(S_Item, f_counts, &sd_type_list),
};
static const sd_type T_Item = SD_STRUCT_TYPE("Item", S_Item, F_Item);

/* ---- prototypes ---- */

static sd_int sd_f_total(S_Item l_it);
static void sd_m_Item__add(S_Item *self, sd_int l_n);
static sd_err sd_f_worker(sd_int l_id, sd_list *l_items, sd_chan *l_out);
static sd_err sd_f_check(sd_int l_n);
static sd_err sd_f_main(sd_list *l_args);

/* ---- functions ---- */

static sd_int sd_f_total(S_Item l_it) { /* l_it is borrowed */
  sd_int l_sum = SD_INT(0);
  for (sd_int l_i = SD_INT(0);;) {
    if (!sd_int_lt(l_i, sd_list_len(l_it.f_counts))) break;
    /* sum = sum + it.counts[i]: the element is read borrowed */
    sd_int t1 = sd_int_add(l_sum, SD_LIST_GET(l_it.f_counts, sd_int, l_i));
    sd_int_release(l_sum);
    l_sum = t1;
    /* i++ */
    sd_int t2 = sd_int_add(l_i, SD_INT(1));
    sd_int_release(l_i);
    l_i = t2;
  }
  return l_sum; /* moved out: no retain, no release */
}

/* A mut method gets a pointer to the receiver's place. */
static void sd_m_Item__add(S_Item *self, sd_int l_n) {
  SD_LIST_PUSH(&self->f_counts, sd_int, sd_int_retain(l_n)); /* push consumes: retain the borrowed n */
}

/* defer fmt.eprintln(<string>): the env holds the evaluated argument. */
typedef struct { sd_str a0; } D1_env;
static void D1_thunk(void *p) {
  D1_env *e = p;
  sd_fmt_eprintln(e->a0);
  sd_str_release(e->a0);
}

static sd_err sd_f_worker(sd_int l_id, sd_list *l_items, sd_chan *l_out) {
  size_t dmark = sd_defer_mark();
  {
    sd_str t1 = sd_int_to_str(l_id);
    sd_str parts[] = {SD_STR_LIT("worker "), t1, SD_STR_LIT(" done")};
    D1_env env = {sd_str_concat_n(parts, 3)};
    sd_str_release(t1);
    sd_defer_push(D1_thunk, &env, sizeof env);
  }
  for (sd_int l_i = SD_INT(0);;) {
    if (!sd_int_lt(l_i, sd_list_len(l_items))) break;
    S_Item *t0 = &SD_LIST_GET(l_items, S_Item, l_i); /* borrowed element */
    sd_str t1 = sd_int_to_str(l_id);
    sd_int t2 = sd_f_total(*t0);
    sd_str t3 = sd_int_to_str(t2);
    sd_str parts[] = {t1, SD_STR_LIT(":"), t0->f_name, SD_STR_LIT("="), t3};
    sd_str t4 = sd_str_concat_n(parts, 5);
    sd_chan_send(l_out, &t4); /* borrowed: the receiver gets its own copy */
    sd_str_release(t4);
    sd_str_release(t3);
    sd_int_release(t2);
    sd_str_release(t1);
    sd_int t5 = sd_int_add(l_i, SD_INT(1));
    sd_int_release(l_i);
    l_i = t5;
  }
  sd_defer_run(dmark);
  return SD_ERR_NULL;
}

/* defer fmt.println("check done"): a literal needs no env. */
static void D2_thunk(void *p) {
  (void)p;
  sd_fmt_println(SD_STR_LIT("check done"));
}

static sd_err sd_f_check(sd_int l_n) {
  size_t dmark = sd_defer_mark();
  sd_defer_push(D2_thunk, NULL, 0);
  if (sd_int_gt(l_n, SD_INT(100))) {
    sd_str t1 = sd_int_to_str(l_n);
    sd_str t2 = sd_str_concat(SD_STR_LIT("too big: "), t1);
    sd_err r = sd_errors_new(t2);
    sd_str_release(t2);
    sd_str_release(t1);
    sd_defer_run(dmark);
    return r;
  }
  sd_defer_run(dmark);
  return SD_ERR_NULL;
}

/* spawn worker(...): an argument struct, its descriptor and a trampoline. */
typedef struct {
  sd_int a0;
  sd_list *a1;
  sd_chan *a2;
} A1_worker;
static const sd_field F_A1_worker[] = {
  SD_FIELD(A1_worker, a0, &sd_type_int),
  SD_FIELD(A1_worker, a1, &sd_type_list),
  SD_FIELD(A1_worker, a2, &sd_type_chan),
};
static const sd_type T_A1_worker = SD_STRUCT_TYPE("spawn worker", A1_worker, F_A1_worker);
static sd_err G1_worker(void *p) {
  A1_worker *a = p; /* the goroutine's own deep copy, owned by the runtime */
  return sd_f_worker(a->a0, a->a1, a->a2);
}

static void D3_thunk(void *p) {
  (void)p;
  sd_fmt_println(SD_STR_LIT("main: deferred"));
}

static sd_err sd_f_main(sd_list *l_args) {
  size_t dmark = sd_defer_mark();
  sd_defer_push(D3_thunk, NULL, 0);

  /* []Item items = []Item{Item{name: "a", counts: []int{1, 2}}, Item{...}} */
  sd_list *l_items = sd_list_new(&T_Item, 2);
  {
    sd_list *c = sd_list_new(&sd_type_int, 2);
    SD_LIST_PUSH(&c, sd_int, SD_INT(1));
    SD_LIST_PUSH(&c, sd_int, SD_INT(2));
    SD_LIST_PUSH(&l_items, S_Item, ((S_Item){SD_STR_LIT("a"), c}));
    SD_LIST_PUSH(&l_items, S_Item, ((S_Item){SD_STR_LIT("b"), sd_list_new(&sd_type_int, 0)}));
  }
  /* []Item other = items: copying from a place retains */
  sd_list *l_other = sd_list_retain(l_items);
  /* other[1].add(40): the place is made unique on the way down */
  sd_m_Item__add((S_Item *)sd_list_at_mut(&l_other, SD_INT(1)), SD_INT(40));
  sd_m_Item__add((S_Item *)sd_list_at_mut(&l_other, SD_INT(1)), SD_INT_BIGLIT("1267650600228229401496703205376"));
  {
    sd_str t1 = sd_int_to_str(sd_list_len(SD_LIST_GET(l_items, S_Item, SD_INT(1)).f_counts));
    sd_str t2 = sd_int_to_str(sd_list_len(SD_LIST_GET(l_other, S_Item, SD_INT(1)).f_counts));
    sd_str parts[] = {SD_STR_LIT("items[1] has "), t1, SD_STR_LIT(", other[1] has "), t2};
    sd_str t3 = sd_str_concat_n(parts, 4);
    sd_fmt_println(t3);
    sd_str_release(t3);
    sd_str_release(t2);
    sd_str_release(t1);
  }
  sd_chan *l_out = sd_chan_new(&sd_type_str, SD_INT(10));

  /* error err = group { spawn worker(1, items, out); spawn worker(2, other, out) } */
  sd_err l_err;
  {
    sd_group *g = sd_group_begin();
    {
      A1_worker a = {SD_INT(1), l_items, l_out}; /* borrowed: sd_spawn deep-copies */
      sd_spawn(g, G1_worker, &T_A1_worker, &a);
    }
    {
      A1_worker a = {SD_INT(2), l_other, l_out};
      sd_spawn(g, G1_worker, &T_A1_worker, &a);
    }
    l_err = sd_group_wait(g);
  }
  if (!sd_err_is_null(l_err)) {
    /* return err: release every other live local, move err out */
    sd_chan_release(l_out);
    sd_list_release(l_other);
    sd_list_release(l_items);
    sd_defer_run(dmark);
    return l_err;
  }
  sd_chan_close(l_out);

  sd_list *l_lines = sd_list_new(&sd_type_str, 0);
  for (;;) {
    sd_str l_s; /* string? */
    if (!sd_chan_receive(l_out, &l_s)) l_s = NULL;
    if (l_s == NULL) break; /* (nothing to release: s is null) */
    SD_LIST_PUSH(&l_lines, sd_str, sd_str_retain(l_s));
    sd_str_release(l_s); /* end of the loop body's scope */
  }
  sd_list_sort(&l_lines);
  {
    sd_str t1 = sd_list_join(l_lines, SD_STR_LIT(" "));
    sd_fmt_println(t1);
    sd_str_release(t1);
  }

  /* map[string]int totals; totals[other[i].name] = total(other[i]) */
  sd_map *l_totals = sd_map_new(&sd_type_str, &sd_type_int);
  for (sd_int l_i = SD_INT(0);;) {
    if (!sd_int_lt(l_i, sd_list_len(l_other))) break;
    S_Item *t0 = &SD_LIST_GET(l_other, S_Item, l_i);
    sd_str k = sd_str_retain(t0->f_name); /* set consumes the key */
    sd_int v = sd_f_total(*t0);           /* and the value (owned already) */
    sd_map_set(&l_totals, &k, &v);
    sd_int t5 = sd_int_add(l_i, SD_INT(1));
    sd_int_release(l_i);
    l_i = t5;
  }
  /* int? b = totals["b"] */
  sd_int l_b;
  {
    sd_str key = SD_STR_LIT("b");
    sd_int *p = sd_map_get(l_totals, &key);
    l_b = p ? sd_int_retain(*p) : SD_INT_NULL;
  }
  if (!sd_int_is_null(l_b)) {
    sd_str t1 = sd_int_to_str(l_b);
    sd_str t2 = sd_str_concat(SD_STR_LIT("b total: "), t1);
    sd_fmt_println(t2);
    sd_str_release(t2);
    sd_str_release(t1);
  }

  /* error e = check(total(other[1])) */
  sd_err l_e;
  {
    sd_int t1 = sd_f_total(SD_LIST_GET(l_other, S_Item, SD_INT(1)));
    l_e = sd_f_check(t1);
    sd_int_release(t1);
  }
  if (!sd_err_is_null(l_e)) {
    sd_str t1 = sd_err_message(l_e);
    sd_str t2 = sd_str_concat(SD_STR_LIT("check failed: "), t1);
    sd_fmt_println(t2);
    sd_str_release(t2);
    sd_str_release(t1);
  }

  /* return null: release locals in reverse order, run defers */
  sd_err_release(l_e);
  sd_int_release(l_b);
  sd_map_release(l_totals);
  sd_list_release(l_lines);
  sd_err_release(l_err);
  sd_chan_release(l_out);
  sd_list_release(l_other);
  sd_list_release(l_items);
#if defined(SD_COUNT_OBJECTS)
  /* still alive: the cached big literal, and args (a list of argc strings) */
  int64_t expected = 1 + 1 + l_args->len;
  if (sd_live_objects() != expected) sd_crash("example leaked %lld objects", (long long)(sd_live_objects() - expected));
#endif
  sd_defer_run(dmark);
  return SD_ERR_NULL;
}

int main(int argc, char **argv) { return sd_main(argc, argv, sd_f_main); }
