/* Lists, maps, boxes, type descriptors, copy-on-write, interfaces, deep
 * copies, and an allocation stress test. Built with -DSD_COUNT_OBJECTS so
 * that leaks show up as a non-zero live object count. */
#include "check.h"

#include <sys/resource.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

/* ---- a struct with counted fields, as a code generator would emit it ---- */
typedef struct {
  sd_str name;
  sd_list *tags; /* []string */
  sd_int age;
  double score;
} S_User;
static const sd_field F_User[] = {
  SD_FIELD(S_User, name, &sd_type_str),
  SD_FIELD(S_User, tags, &sd_type_list),
  SD_FIELD(S_User, age, &sd_type_int),
  SD_FIELD(S_User, score, &sd_type_float),
};
static const sd_type T_User = SD_STRUCT_TYPE("User", S_User, F_User);

/* ---- an enum: type Shape enum { Circle(float r), Rect(float w, float h), Named(string n), Empty } */
typedef struct {
  int32_t tag;
  union {
    struct { double r; } Circle;
    struct { double w, h; } Rect;
    struct { sd_str n; } Named;
  } u;
} E_Shape;
enum { Shape_Circle, Shape_Rect, Shape_Named, Shape_Empty };
static const sd_field F_Circle[] = {SD_FIELD(E_Shape, u.Circle.r, &sd_type_float)};
static const sd_field F_Rect[] = {SD_FIELD(E_Shape, u.Rect.w, &sd_type_float), SD_FIELD(E_Shape, u.Rect.h, &sd_type_float)};
static const sd_field F_Named[] = {SD_FIELD(E_Shape, u.Named.n, &sd_type_str)};
static const sd_variant V_Shape[] = {
  {"Circle", F_Circle, 1}, {"Rect", F_Rect, 2}, {"Named", F_Named, 1}, {"Empty", NULL, 0}};
static const sd_type T_Shape = SD_ENUM_TYPE("Shape", E_Shape, V_Shape);
SD_BOX_STATIC(Shape_Empty_box, &T_Shape, E_Shape, {.tag = Shape_Empty});

static sd_box *mk_circle(double r) {
  E_Shape v = {.tag = Shape_Circle, .u.Circle = {r}};
  return sd_box_new(&T_Shape, &v);
}
static sd_box *mk_named(const char *n) {
  E_Shape v = {.tag = Shape_Named, .u.Named = {sd_str_from_cstr(n)}};
  return sd_box_new(&T_Shape, &v);
}

static S_User mk_user(const char *name, int age) {
  S_User u = {sd_str_from_cstr(name), sd_list_new(&sd_type_str, 0), SD_INT(age), 1.5};
  return u;
}

static void test_lists(void) {
  sd_list *xs = sd_list_new(&sd_type_int, 0);
  for (int i = 0; i < 1000; i++) SD_LIST_PUSH(&xs, sd_int, SD_INT(i));
  CHECK(xs->len == 1000 && sd_int_eq(SD_LIST_GET(xs, sd_int, SD_INT(999)), SD_INT(999)));
  /* value semantics: b = a; b.push(x) leaves a alone */
  sd_list *ys = sd_list_retain(xs);
  sd_int big = sd_int_from_lit("1000000000000000000000000");
  sd_list_push(&ys, &big); /* consumes big */
  CHECK(xs != ys && xs->len == 1000 && ys->len == 1001 && xs->rc == 1 && ys->rc == 1);
  /* pop */
  sd_int out;
  CHECK(sd_list_pop(&ys, &out));
  CHECK_INT(out, "1000000000000000000000000");
  sd_list *empty = sd_list_new(&sd_type_int, 0);
  CHECK(!sd_list_pop(&empty, &out));
  sd_list_release(empty);
  /* insert / remove / set */
  sd_list_insert(&ys, SD_INT(0), (sd_int[]){SD_INT(-1)});
  sd_list_insert(&ys, SD_INT(1001), (sd_int[]){SD_INT(5000)});
  CHECK(ys->len == 1002 && sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(0)), SD_INT(-1)));
  CHECK(sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(1001)), SD_INT(5000)));
  sd_list_remove(&ys, SD_INT(0));
  CHECK(sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(0)), SD_INT(0)));
  sd_list_set(&ys, SD_INT(3), (sd_int[]){SD_INT(33)});
  *(sd_int *)sd_list_at_mut(&ys, SD_INT(4)) = SD_INT(44);
  CHECK(sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(3)), SD_INT(33)) && sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(4)), SD_INT(44)));
  /* contains / index / slice / equal */
  CHECK(sd_list_contains(ys, (sd_int[]){SD_INT(5000)}));
  CHECK(!sd_list_contains(ys, (sd_int[]){SD_INT(3)}));
  CHECK(sd_int_eq(sd_list_index(ys, (sd_int[]){SD_INT(44)}), SD_INT(4)));
  CHECK(sd_int_is_null(sd_list_index(ys, (sd_int[]){SD_INT(-7)})));
  sd_list *sl = sd_list_slice(ys, SD_INT(10), SD_INT(20));
  CHECK(sl->len == 10 && sd_int_eq(SD_LIST_GET(sl, sd_int, SD_INT(0)), SD_INT(10)));
  sd_list *sl2 = sd_list_slice(xs, SD_INT(10), SD_INT(20));
  CHECK(sd_list_equal(sl, sl2));
  sd_list_set(&sl2, SD_INT(0), (sd_int[]){SD_INT(0)});
  CHECK(!sd_list_equal(sl, sl2));
  sd_list_release(sl);
  sd_list_release(sl2);
  /* reverse and sort */
  sd_list_reverse(&ys);
  CHECK(sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(0)), SD_INT(5000)));
  sd_list_sort(&ys);
  CHECK(sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(0)), SD_INT(0)));
  CHECK(sd_int_eq(SD_LIST_GET(ys, sd_int, SD_INT(ys->len - 1)), SD_INT(5000)));
  for (int64_t i = 1; i < ys->len; i++)
    CHECK(sd_int_le(SD_LIST_GET(ys, sd_int, SD_INT(i - 1)), SD_LIST_GET(ys, sd_int, SD_INT(i))));
  sd_list_release(xs);
  sd_list_release(ys);

  /* strings: sort and join */
  sd_list *ss = sd_str_split(SD_STR_LIT("pear apple fig banana apple"), SD_STR_LIT(" "));
  sd_list *ss2 = sd_list_retain(ss);
  sd_list_sort(&ss2);
  CHECK_SDSTR(sd_list_join(ss2, SD_STR_LIT(",")), "apple,apple,banana,fig,pear");
  CHECK_SDSTR(sd_list_join(ss, SD_STR_LIT(",")), "pear,apple,fig,banana,apple");
  sd_list_release(ss);
  sd_list_release(ss2);

  /* floats and bytes */
  sd_list *fs = sd_list_new(&sd_type_float, 0);
  double fv[] = {3.5, -1.0, 2.25, 0.0};
  for (int i = 0; i < 4; i++) SD_LIST_PUSH(&fs, double, fv[i]);
  sd_list_sort(&fs);
  CHECK(SD_LIST_GET(fs, double, SD_INT(0)) == -1.0 && SD_LIST_GET(fs, double, SD_INT(3)) == 3.5);
  sd_list_release(fs);
  sd_list *bs = sd_str_bytes(SD_STR_LIT("seed"));
  sd_list_sort(&bs);
  CHECK_SDSTR(sd_str_from_bytes(bs), "dees");
  sd_list_release(bs);
}

/* sortBy with a Seed function value fn(User, User) bool, through an adapter */
static bool olderThan(S_User a, S_User b) { return sd_int_gt(a.age, b.age); }
static bool less_User(void *ctx, const void *a, const void *b) {
  bool (*f)(S_User, S_User) = (bool (*)(S_User, S_User))ctx;
  return f(*(const S_User *)a, *(const S_User *)b);
}

static void test_structs(void) {
  sd_list *users = sd_list_new(&T_User, 0);
  const char *names[] = {"ann", "bob", "cy", "dee", "eve"};
  int ages[] = {30, 40, 30, 50, 40};
  for (int i = 0; i < 5; i++) {
    S_User u = mk_user(names[i], ages[i]);
    sd_list_push(&users, &u);
  }
  /* users2 = users; users2[0].tags.push("x") must not change users */
  sd_list *users2 = sd_list_retain(users);
  S_User *u0 = sd_list_at_mut(&users2, SD_INT(0));
  SD_LIST_PUSH(&u0->tags, sd_str, SD_STR_LIT("x"));
  CHECK(SD_LIST_GET(users, S_User, SD_INT(0)).tags->len == 0);
  CHECK(SD_LIST_GET(users2, S_User, SD_INT(0)).tags->len == 1);
  /* the inner lists of the other users are shared (rc 2) until written */
  CHECK(SD_LIST_GET(users, S_User, SD_INT(1)).tags == SD_LIST_GET(users2, S_User, SD_INT(1)).tags);
  CHECK(SD_LIST_GET(users, S_User, SD_INT(1)).tags->rc == 2);
  CHECK(!sd_list_equal(users, users2));
  S_User *u0b = sd_list_at_mut(&users2, SD_INT(0));
  sd_str popped;
  CHECK(sd_list_pop(&u0b->tags, &popped));
  sd_str_release(popped);
  CHECK(sd_list_equal(users, users2)); /* by contents */
  /* sortBy: stable */
  sd_list_sort_by(&users2, less_User, (void *)olderThan);
  const char *want[] = {"dee", "bob", "eve", "ann", "cy"};
  for (int i = 0; i < 5; i++) CHECK_STREQ(sd_str_cstr(SD_LIST_GET(users2, S_User, SD_INT(i)).name), want[i]);
  CHECK_STREQ(sd_str_cstr(SD_LIST_GET(users, S_User, SD_INT(0)).name), "ann");
  /* contains with struct equality */
  S_User probe = mk_user("cy", 30);
  CHECK(sd_list_contains(users, &probe));
  probe.score = 2.0;
  CHECK(!sd_list_contains(users, &probe));
  sd_release(&T_User, &probe);
  /* deep clone shares nothing */
  sd_list *deep = sd_list_clone_deep(users2);
  CHECK(sd_list_equal(deep, users2));
  for (int i = 0; i < 5; i++) {
    S_User *a = &SD_LIST_GET(deep, S_User, SD_INT(i)), *b = &SD_LIST_GET(users2, S_User, SD_INT(i));
    CHECK(a->name != b->name && a->tags != b->tags && a->name->rc == 1 && a->tags->rc == 1);
  }
  sd_list_release(deep);
  sd_list_release(users);
  sd_list_release(users2);
}

static void test_maps(void) {
  sd_map *m = sd_map_new(&sd_type_str, &sd_type_int);
  const char *words[] = {"the", "quick", "brown", "fox", "jumps", "over", "the", "lazy", "dog", "the"};
  for (int i = 0; i < 10; i++) {
    sd_str k = sd_str_from_cstr(words[i]);
    sd_int *have = sd_map_get(m, &k);
    sd_int v = have ? sd_int_add(*have, SD_INT(1)) : SD_INT(1);
    sd_map_set(&m, &k, &v); /* consumes k and v */
  }
  CHECK(sd_int_eq(sd_map_len(m), SD_INT(8)));
  sd_str the = SD_STR_LIT("the");
  CHECK(sd_int_eq(*(sd_int *)sd_map_get(m, &the), SD_INT(3)));
  sd_list *keys = sd_map_keys(m);
  CHECK_SDSTR(sd_list_join(keys, SD_STR_LIT(" ")), "the quick brown fox jumps over lazy dog");
  sd_list_release(keys);
  /* copy-on-write */
  sd_map *m2 = sd_map_retain(m);
  sd_str fox = SD_STR_LIT("fox");
  sd_map_delete(&m2, &fox);
  CHECK(m != m2 && sd_int_eq(sd_map_len(m), SD_INT(8)) && sd_int_eq(sd_map_len(m2), SD_INT(7)));
  CHECK(sd_map_get(m2, &fox) == NULL && sd_map_get(m, &fox) != NULL);
  /* re-adding a deleted key goes to the end; replacing keeps the position */
  sd_map_set(&m2, &fox, (sd_int[]){SD_INT(9)});
  sd_str quick = SD_STR_LIT("quick");
  sd_map_set(&m2, &quick, (sd_int[]){SD_INT(7)});
  keys = sd_map_keys(m2);
  CHECK_SDSTR(sd_list_join(keys, SD_STR_LIT(" ")), "the quick brown jumps over lazy dog fox");
  sd_list_release(keys);
  CHECK(!sd_map_equal(m, m2));
  sd_map_set(&m2, &quick, (sd_int[]){SD_INT(1)});
  sd_map_set(&m2, &fox, (sd_int[]){SD_INT(1)});
  CHECK(sd_map_equal(m, m2)); /* same entries, different order */
  /* iteration */
  int64_t pos = 0, n = 0;
  void *k, *v;
  while (sd_map_next(m2, &pos, &k, &v)) n++;
  CHECK(n == 8);
  sd_map_release(m);
  sd_map_release(m2);

  /* int keys including big ones; many entries with churn */
  sd_map *mi = sd_map_new(&sd_type_int, &sd_type_str);
  for (int i = 0; i < 100000; i++) {
    sd_int key = sd_int_mul(SD_INT(i), SD_INT(i % 3 == 0 ? INT64_C(1) << 61 : 1));
    sd_str val = sd_int_to_str(SD_INT(i));
    sd_map_set(&mi, &key, &val);
    if (i % 2) {
      sd_int dk = sd_int_mul(SD_INT(i - 1), SD_INT((i - 1) % 3 == 0 ? INT64_C(1) << 61 : 1));
      sd_map_delete(&mi, &dk);
      sd_int_release(dk);
    }
  }
  CHECK(sd_int_eq(sd_map_len(mi), SD_INT(50000)));
  sd_int k3 = sd_int_mul(SD_INT(99999), SD_INT(INT64_C(1) << 61));
  sd_str *got = sd_map_get(mi, &k3);
  CHECK(got && strcmp(sd_str_cstr(*got), "99999") == 0);
  sd_int_release(k3);
  sd_map *mi2 = sd_map_clone_deep(mi);
  CHECK(sd_map_equal(mi, mi2));
  sd_map_release(mi);
  sd_map_release(mi2);

  /* byte and bool keys */
  sd_map *mb = sd_map_new(&sd_type_bool, &sd_type_byte);
  sd_map_set(&mb, &(bool){true}, &(uint8_t){1});
  sd_map_set(&mb, &(bool){false}, &(uint8_t){2});
  sd_map_set(&mb, &(bool){true}, &(uint8_t){3});
  CHECK(sd_int_eq(sd_map_len(mb), SD_INT(2)) && *(uint8_t *)sd_map_get(mb, &(bool){true}) == 3);
  keys = sd_map_keys(mb);
  CHECK(keys->et == &sd_type_bool && SD_LIST_GET(keys, bool, SD_INT(0)) == true);
  sd_list_release(keys);
  sd_map_release(mb);

  /* map of lists: m[k].push through get_mut */
  sd_map *ml = sd_map_new(&sd_type_str, &sd_type_list);
  sd_str key = SD_STR_LIT("k");
  sd_list *lst = sd_list_new(&sd_type_int, 0);
  sd_map_set(&ml, &key, &lst);
  sd_map *ml2 = sd_map_retain(ml);
  sd_list **pl = sd_map_get_mut(&ml2, &key);
  SD_LIST_PUSH(pl, sd_int, SD_INT(1));
  CHECK((*(sd_list **)sd_map_get(ml, &key))->len == 0 && (*(sd_list **)sd_map_get(ml2, &key))->len == 1);
  sd_map_release(ml);
  sd_map_release(ml2);
}

/* ---- a user type implementing error, and an interface ---- */
typedef struct { sd_str path; } S_NotFound;
static const sd_field F_NotFound[] = {SD_FIELD(S_NotFound, path, &sd_type_str)};
static const sd_type T_NotFound = SD_STRUCT_TYPE("NotFound", S_NotFound, F_NotFound);
static sd_str NotFound_message(const void *self) {
  const S_NotFound *e = self;
  sd_str parts[] = {SD_STR_LIT("not found: "), e->path};
  return sd_str_concat_n(parts, 2);
}
static const sd_error_vtable VT_NotFound_error = {{"NotFound"}, NotFound_message};

static void test_boxes_ifaces(void) {
  /* enums in boxes */
  sd_box *c1 = mk_circle(2.0), *c2 = mk_circle(2.0), *n1 = mk_named("x"), *n2 = mk_named("y");
  CHECK(sd_box_equal(c1, c2) && !sd_box_equal(c1, n1) && !sd_box_equal(n1, n2));
  sd_box *e1 = SD_BOX_REF(Shape_Empty_box), *em2 = SD_BOX_REF(Shape_Empty_box);
  CHECK(sd_box_equal(e1, em2) && !sd_box_equal(e1, c1));
  sd_box_release(sd_box_retain(e1));
  CHECK(e1->rc == SD_RC_IMMORTAL);
  /* list of enums: release frees payload strings */
  sd_list *shapes = sd_list_new(&sd_type_box, 0);
  sd_list_push(&shapes, &c1);
  sd_list_push(&shapes, &n1);
  SD_LIST_PUSH(&shapes, sd_box *, e1);
  CHECK(sd_list_contains(shapes, &c2));
  sd_list *shapes2 = sd_list_clone_deep(shapes);
  CHECK(sd_list_equal(shapes, shapes2));
  CHECK(SD_LIST_GET(shapes2, sd_box *, SD_INT(2)) == e1); /* immortal: shared */
  sd_list_release(shapes);
  sd_list_release(shapes2);
  sd_box_release(c2);
  sd_box_release(n2);
  /* nullable struct (box) with copy-on-write */
  S_User u = mk_user("ann", 30);
  sd_box *b = sd_box_new(&T_User, &u);
  sd_box *b2 = sd_box_retain(b);
  S_User *w = sd_box_mut(&b2);
  sd_int_release(w->age);
  w->age = SD_INT(31);
  CHECK(b != b2 && sd_int_eq(SD_BOX_GET(b, S_User).age, SD_INT(30)));
  CHECK(SD_BOX_GET(b, S_User).name == SD_BOX_GET(b2, S_User).name); /* fields shared */
  sd_box_release(b);
  sd_box_release(b2);
  /* errors */
  sd_err e = sd_errors_new(SD_STR_LIT("disk full"));
  CHECK_SDSTR(sd_err_message(e), "disk full");
  sd_err e2 = sd_errorf("disk %s", "full");
  CHECK(sd_err_equal(e, e2) && !sd_err_equal(e, SD_ERR_NULL) && sd_err_equal(SD_ERR_NULL, SD_ERR_NULL));
  S_NotFound nf = {sd_str_from_cstr("b.txt")};
  sd_err e3 = sd_iface_new(&VT_NotFound_error.hdr, &T_NotFound, &nf);
  CHECK_SDSTR(sd_err_message(e3), "not found: b.txt");
  CHECK(!sd_err_equal(e, e3));
  sd_err e4;
  sd_clone_deep(&sd_type_err, &e4, &e3);
  CHECK(e4.obj != e3.obj && sd_iface_equal(e3, e4));
  /* errors in a list */
  sd_list *errs = sd_list_new(&sd_type_err, 0);
  sd_list_push(&errs, &e3);
  sd_list_push(&errs, (sd_err[]){SD_ERR_NULL});
  CHECK(sd_list_contains(errs, &e4) && sd_list_contains(errs, (sd_err[]){SD_ERR_NULL}));
  sd_list_release(errs);
  sd_err_release(e);
  sd_err_release(e2);
  sd_err_release(e4);
}

/* Resident memory in bytes, now. */
static size_t rss_now(void) {
#if defined(__APPLE__)
  struct mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
  return (size_t)info.resident_size;
#else
  FILE *f = fopen("/proc/self/statm", "r");
  long pages = 0, res = 0;
  if (f) {
    if (fscanf(f, "%ld %ld", &pages, &res) != 2) res = 0;
    fclose(f);
  }
  return (size_t)res * (size_t)sysconf(_SC_PAGESIZE);
#endif
}

/* Millions of small allocations, all freed: the live count returns to the
 * start and resident memory stays flat. */
static void test_stress(long rounds) {
  int64_t live0 = sd_live_objects();
  size_t rss_after_warmup = 0;
  for (long r = 0; r < rounds; r++) {
    if (r == rounds / 10) rss_after_warmup = rss_now();
    sd_list *l = sd_list_new(&sd_type_str, 0);
    for (int i = 0; i < 20; i++) SD_LIST_PUSH(&l, sd_str, sd_int_to_str(SD_INT(r * 20 + i + 1000)));
    sd_map *m = sd_map_new(&sd_type_int, &sd_type_list);
    sd_int big = sd_int_mul(SD_INT(r), SD_INT(INT64_C(1) << 61));
    sd_list *l2 = sd_list_retain(l);
    sd_map_set(&m, &big, &l2);
    S_User u = mk_user("x", (int)r);
    SD_LIST_PUSH(&u.tags, sd_str, SD_STR_LIT("t"));
    sd_box *b = sd_box_new(&T_User, &u);
    sd_list_release(l);
    sd_map_release(m);
    sd_box_release(b);
  }
  size_t rss_end = rss_now();
  CHECK(sd_live_objects() == live0);
  long growth_kb = (long)((rss_end > rss_after_warmup ? rss_end - rss_after_warmup : 0) / 1024);
  printf("stress: %ld rounds (~%ld allocations), RSS growth after warm-up: %ld KB\n", rounds, rounds * 27,
         growth_kb);
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SD_TEST_ASAN 1
#endif
#endif
#if !defined(SD_TEST_ASAN) && !defined(__SANITIZE_ADDRESS__)
  CHECK(growth_kb < 4096); /* (ASan's quarantine keeps freed memory resident) */
#endif
}

/* Dropping long chains frees them without recursion (a recursive free of
 * a million-node list overflows the stack): a linked list of boxes
 * (type Node struct { int v; Node? next }), a list nested in lists, and a
 * chain through maps. */
typedef struct { sd_int v; sd_box *next; } S_Node;
static const sd_field F_Node[] = {SD_FIELD(S_Node, v, &sd_type_int), SD_FIELD(S_Node, next, &sd_type_box)};
static const sd_type T_Node = SD_STRUCT_TYPE("Node", S_Node, F_Node);

static void test_long_chains(void) {
  int64_t live0 = sd_live_objects();
  enum { N = 1000000 };
  sd_box *head = NULL;
  for (int i = 0; i < N; i++) {
    S_Node n = {SD_INT(i), head};
    head = sd_box_new(&T_Node, &n);
  }
  sd_box_release(head);
  CHECK(sd_live_objects() == live0);
  sd_list *l = sd_list_new(&sd_type_list, 1);
  for (int i = 0; i < N; i++) {
    sd_list *outer = sd_list_new(&sd_type_list, 1);
    SD_LIST_PUSH(&outer, sd_list *, l);
    l = outer;
  }
  sd_list_release(l);
  CHECK(sd_live_objects() == live0);
  sd_map *m = sd_map_new(&sd_type_int, &sd_type_map);
  for (int i = 0; i < N / 10; i++) {
    sd_map *outer = sd_map_new(&sd_type_int, &sd_type_map);
    sd_int k = SD_INT(i);
    sd_map_set(&outer, &k, &m);
    m = outer;
  }
  sd_map_release(m);
  CHECK(sd_live_objects() == live0);
}

int main(int argc, char **argv) {
  int64_t live0 = sd_live_objects();
  test_lists();
  test_structs();
  test_maps();
  test_boxes_ifaces();
  test_long_chains();
  CHECK(sd_live_objects() == live0);
  if (sd_live_objects() != live0) fprintf(stderr, "leaked %lld objects\n", (long long)(sd_live_objects() - live0));
  test_stress(argc > 1 ? atol(argv[1]) : 200000);
  return check_done("test_mem");
}
