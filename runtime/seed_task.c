/* Goroutines (one OS thread each), crashes and defer, groups, channels,
 * select, timers and program entry.
 *
 * Locking order (never acquire against it):
 *   channel locks (several at once: in address order)
 *     -> task->mu
 *   group->mu -> task->mu -> (that task's waiting group)->mu -> ...  (down the tree)
 *   timer lock is never held while taking another lock.
 */
#include "seed_internal.h"

#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#ifndef SD_GOROUTINE_STACK
#define SD_GOROUTINE_STACK ((size_t)8 << 20)
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define SD_NO_FAULT_HANDLER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SD_NO_FAULT_HANDLER 1
#endif

typedef struct sd_task sd_task;
typedef struct sd_sel sd_sel;

/* ------------------------------------------------------------- structures */

typedef struct sd_frame {
  sigjmp_buf jb;
  struct sd_frame *prev;
  size_t mark; /* cleanup-stack depth when the frame was entered */
} sd_frame;

/* One entry of a goroutine's cleanup stack: a deferred call (fn != NULL), an
 * active group (group != NULL), or a tombstone (both NULL). */
typedef struct sd_cleanup {
  void (*fn)(void *env);
  sd_group *group;
  size_t off, size; /* env bytes in the arena */
} sd_cleanup;

enum { SD_RAISE_NONE = 0, SD_RAISE_CRASH, SD_RAISE_STOP };

struct sd_task {
  sd_frame *frame;   /* innermost protected frame */
  int raise;         /* pending raise kind */
  char *msg;         /* crash message (malloc'ed unless msg_static) */
  bool msg_static;

  sd_cleanup *cl;
  size_t ncl, capcl;
  char *arena;
  size_t arena_len, arena_cap;

  pthread_mutex_t mu; /* protects wake, sleeping, sel, waiting_group */
  pthread_cond_t cv;
  bool wake, sleeping;
  sd_sel *sel;
  sd_group *waiting_group;
  atomic_int cancelled;

  sd_group *group; /* the group this goroutine belongs to (NULL for main) */
  sd_task *next;   /* next member of that group */
  pthread_t thread;
  sd_task_fn body;
  void *args;
  const sd_type *args_type;
  uint64_t rng;
  void *altstack;
  void (**atexit_fns)(void); /* sd_goroutine_atexit, run when the goroutine ends */
  size_t natexit, capatexit;
};

struct sd_group {
  pthread_mutex_t mu;
  sd_task *members;
  int64_t running;
  bool failed;
  sd_err err;
  sd_task *waiter;
};

#define SEL_CANCELLED (-1)

struct sd_sel {
  atomic_int done; /* 0 pending, k + 1 case k fired, SEL_CANCELLED */
  sd_task *task;
};

typedef struct sd_waiter {
  struct sd_waiter *next, *prev;
  sd_sel *sel;
  int idx;
  void *val;   /* send: the sender's value (read by the receiver); recv: destination */
  int result;  /* 1: value transferred, 0: channel closed */
  bool queued;
} sd_waiter;

typedef struct {
  sd_waiter *head, *tail;
} sd_waitq;

struct sd_chan {
  _Atomic int64_t rc;
  pthread_mutex_t mu;
  const sd_type *et;
  int64_t cap, count, head;
  char *buf;
  bool closed;
  sd_waitq recvq, sendq;
};

static _Thread_local sd_task *sd_cur;
static atomic_long sd_running; /* goroutines not parked, plus pending timers */
static atomic_ulong sd_seed_counter;
static bool sd_fault_installed;

/* ---------------------------------------------------------------- tasks */

static uint64_t sd_rand(sd_task *t) {
  uint64_t x = t->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  t->rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static sd_task *task_new(void) {
  sd_task *t = calloc(1, sizeof *t);
  if (!t) sd_fatal("out of memory");
  pthread_mutex_init(&t->mu, NULL);
  pthread_cond_init(&t->cv, NULL);
  atomic_init(&t->cancelled, 0);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  t->rng = ((uint64_t)ts.tv_nsec * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)(uintptr_t)t << 7) ^
           (atomic_fetch_add(&sd_seed_counter, 1) * 0xBF58476D1CE4E5B9ULL);
  if (!t->rng) t->rng = 0x853c49e6748fea9bULL;
  return t;
}

static void task_free(sd_task *t) {
  pthread_mutex_destroy(&t->mu);
  pthread_cond_destroy(&t->cv);
  free(t->cl);
  free(t->arena);
  free(t->atexit_fns);
  free(t->msg_static ? NULL : t->msg);
  free(t);
}

/* The current goroutine. A thread that did not start through sd_main or
 * sd_spawn (a unit test, say) gets a root task on first use. */
static sd_task *task_current(void) {
  sd_task *t = sd_cur;
  if (SD_UNLIKELY(!t)) {
    t = task_new();
    atomic_fetch_add(&sd_running, 1);
    sd_cur = t;
  }
  return t;
}

/* ------------------------------------------------- parking and deadlocks */

static SD_NORETURN void sd_deadlock(void) {
  fflush(stdout);
  static const char m[] = "crash: deadlock: all goroutines are blocked\n";
  if (write(2, m, sizeof m - 1) < 0) _exit(2);
  _exit(2);
}

static void running_dec(void) {
  if (atomic_fetch_sub(&sd_running, 1) == 1) sd_deadlock();
}

/* Blocks until someone calls unpark. The caller resets t->wake (under
 * t->mu) before making itself findable by wakers. */
static void park(sd_task *t) {
  pthread_mutex_lock(&t->mu);
  if (!t->wake) {
    t->sleeping = true;
    running_dec();
    while (!t->wake) pthread_cond_wait(&t->cv, &t->mu);
  }
  pthread_mutex_unlock(&t->mu);
}

static void unpark_locked(sd_task *t) {
  t->wake = true;
  if (t->sleeping) {
    t->sleeping = false;
    atomic_fetch_add(&sd_running, 1);
    pthread_cond_signal(&t->cv);
  }
}

static void unpark(sd_task *t) {
  pthread_mutex_lock(&t->mu);
  unpark_locked(t);
  pthread_mutex_unlock(&t->mu);
}

static void reset_wake(sd_task *t) {
  pthread_mutex_lock(&t->mu);
  t->wake = false;
  pthread_mutex_unlock(&t->mu);
}

/* --------------------------------------------------- raising and frames */

static SD_NORETURN void raise_in(sd_task *t, int kind, char *msg) {
  if (t->raise == SD_RAISE_NONE) {
    t->raise = kind;
    t->msg = msg;
    t->msg_static = false;
  } else {
    free(msg); /* the first crash (or stop) wins */
  }
  if (!t->frame) {
    fflush(stdout);
    fprintf(stderr, "crash: %s\n", t->msg ? t->msg : "goroutine stopped");
    sd_exit_process(2);
  }
  siglongjmp(t->frame->jb, 1);
}

void sd_crash(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *m = sd_vfmt(fmt, ap);
  va_end(ap);
  raise_in(task_current(), SD_RAISE_CRASH, m);
}

void sd_crash_index(sd_int i, int64_t len) {
  char *s = sd_int_to_cstr(i);
  sd_crash("index out of range: %s with length %lld", s, (long long)len);
}

void sd_crash_null(void) { sd_crash("null value used"); }

static SD_NORETURN void stop_task(sd_task *t) { raise_in(t, SD_RAISE_STOP, NULL); }

static void check_cancel(sd_task *t) {
  if (SD_UNLIKELY(atomic_load_explicit(&t->cancelled, memory_order_relaxed))) stop_task(t);
}

static void unwind_to(sd_task *t, size_t mark);

/* Runs fn(arg) in a protected frame. Returns SD_RAISE_NONE when fn returned;
 * otherwise the cleanups fn left behind have been run and the pending raise
 * kind is returned (it stays pending in t->raise for the caller). */
static int protect(sd_task *t, void (*fn)(void *), void *arg) {
  sd_frame f;
  f.prev = t->frame;
  f.mark = t->ncl;
  t->frame = &f;
  if (sigsetjmp(f.jb, 1) == 0) {
    fn(arg);
    t->frame = f.prev;
    return SD_RAISE_NONE;
  }
  t->frame = &f;
  unwind_to(t, f.mark);
  t->frame = f.prev;
  return t->raise;
}

/* Takes the pending raise: returns its kind and (malloc'ed) message. */
static int take_raise(sd_task *t, char **msg) {
  int k = t->raise;
  char *m = t->msg;
  if (t->msg_static) m = sd_fmt("%s", m);
  if (msg) *msg = m ? m : sd_fmt("goroutine stopped");
  else free(m);
  t->raise = SD_RAISE_NONE;
  t->msg = NULL;
  t->msg_static = false;
  return k;
}

/* ---------------------------------------------------------------- defer */

static void cl_push(sd_task *t, sd_cleanup e) {
  if (t->ncl == t->capcl) {
    t->capcl = t->capcl ? t->capcl * 2 : 16;
    t->cl = sd_realloc(t->cl, t->capcl * sizeof *t->cl);
  }
  t->cl[t->ncl++] = e;
}

size_t sd_defer_mark(void) { return task_current()->ncl; }

void sd_goroutine_atexit(void (*fn)(void)) {
  sd_task *t = task_current();
  if (t->natexit == t->capatexit) {
    t->capatexit = t->capatexit ? t->capatexit * 2 : 8;
    t->atexit_fns = sd_realloc(t->atexit_fns, t->capatexit * sizeof *t->atexit_fns);
  }
  t->atexit_fns[t->natexit++] = fn;
}

void sd_defer_push(void (*thunk)(void *env), const void *env, size_t size) {
  sd_task *t = task_current();
  size_t sz = (size + 15) & ~(size_t)15;
  if (t->arena_len + sz > t->arena_cap) {
    size_t cap = t->arena_cap ? t->arena_cap * 2 : 1024;
    while (cap < t->arena_len + sz) cap *= 2;
    t->arena = sd_realloc(t->arena, cap);
    t->arena_cap = cap;
  }
  if (size) memcpy(t->arena + t->arena_len, env, size);
  cl_push(t, (sd_cleanup){thunk, NULL, t->arena_len, size});
  t->arena_len += sz;
}

static void group_abort(sd_task *t, sd_group *g);

/* Pops the top entry and runs it. When protected, a crash inside the deferred
 * call is contained (it runs the call's own cleanups and moves on). */
static void run_top_cleanup(sd_task *t, bool protected_) {
  sd_cleanup e = t->cl[--t->ncl];
  if (e.group) {
    t->arena_len = e.off;
    group_abort(t, e.group);
    return;
  }
  if (!e.fn) {
    t->arena_len = e.off;
    return;
  }
  uint64_t local[32];
  void *env = e.size <= sizeof local ? (void *)local : sd_alloc(e.size);
  if (e.size) memcpy(env, t->arena + e.off, e.size);
  t->arena_len = e.off;
  if (protected_) protect(t, e.fn, env);
  else e.fn(env);
  if (env != (void *)local) free(env);
}

void sd_defer_run(size_t mark) {
  sd_task *t = task_current();
  while (t->ncl > mark) run_top_cleanup(t, false);
}

static void unwind_to(sd_task *t, size_t mark) {
  while (t->ncl > mark) run_top_cleanup(t, true);
}

/* --------------------------------------------------------------- groups */

static void group_cancel(sd_group *g);

static void task_cancel(sd_task *m) {
  atomic_store(&m->cancelled, 1);
  pthread_mutex_lock(&m->mu);
  if (m->sel) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&m->sel->done, &expected, SEL_CANCELLED)) unpark_locked(m);
  }
  if (m->waiting_group) group_cancel(m->waiting_group);
  pthread_mutex_unlock(&m->mu);
}

static void group_cancel_locked(sd_group *g) {
  if (g->failed) return;
  g->failed = true;
  for (sd_task *m = g->members; m; m = m->next) task_cancel(m);
}

static void group_cancel(sd_group *g) {
  pthread_mutex_lock(&g->mu);
  group_cancel_locked(g);
  pthread_mutex_unlock(&g->mu);
}

sd_group *sd_group_begin(void) {
  sd_task *t = task_current();
  sd_group *g = calloc(1, sizeof *g);
  if (!g) sd_fatal("out of memory");
  pthread_mutex_init(&g->mu, NULL);
  cl_push(t, (sd_cleanup){NULL, g, t->arena_len, 0});
  return g;
}

typedef struct {
  sd_task *t;
  sd_err *result;
} body_call;

static void run_body(void *p) {
  body_call *bc = p;
  *bc->result = bc->t->body(bc->t->args);
}

static void setup_altstack(sd_task *t) {
  if (!sd_fault_installed) return;
  size_t sz = 64 * 1024;
  t->altstack = sd_alloc(sz);
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = t->altstack;
  ss.ss_size = sz;
  sigaltstack(&ss, NULL);
}

static void teardown_altstack(sd_task *t) {
  if (!t->altstack) return;
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_flags = SS_DISABLE;
  sigaltstack(&ss, NULL);
  free(t->altstack);
  t->altstack = NULL;
}

static void *thread_main(void *p) {
  sd_task *t = p;
  sd_cur = t;
  setup_altstack(t);
  sd_err result = SD_ERR_NULL;
  body_call bc = {t, &result};
  int r = protect(t, run_body, &bc);
  if (r != SD_RAISE_NONE) {
    char *msg;
    take_raise(t, &msg);
    result = r == SD_RAISE_CRASH ? sd_errorf("crash: %s", msg) : SD_ERR_NULL;
    free(msg);
  }
  if (t->args_type) {
    sd_release(t->args_type, t->args);
    free(t->args);
    t->args = NULL;
  }
  while (t->natexit > 0) t->atexit_fns[--t->natexit]();
  sd_group *g = t->group;
  pthread_mutex_lock(&g->mu);
  if (!sd_err_is_null(result)) {
    if (sd_err_is_null(g->err)) {
      g->err = result;
      result = SD_ERR_NULL;
    }
    group_cancel_locked(g);
  }
  sd_task *w = NULL;
  if (--g->running == 0) {
    w = g->waiter;
    g->waiter = NULL;
  }
  pthread_mutex_unlock(&g->mu);
  sd_err_release(result);
  if (w) unpark(w);
  teardown_altstack(t);
  sd_cur = NULL;
  running_dec();
  return NULL;
}

void sd_spawn(sd_group *g, sd_task_fn body, const sd_type *args_type, const void *args) {
  sd_task *parent = task_current();
  sd_task *c = task_new();
  c->body = body;
  c->group = g;
  c->args_type = args_type;
  if (args_type) {
    c->args = sd_alloc(args_type->size);
    sd_clone_deep(args_type, c->args, args);
  }
  pthread_mutex_lock(&g->mu);
  if (g->failed || atomic_load(&parent->cancelled)) {
    /* The group is already failing: the goroutine is not started. */
    pthread_mutex_unlock(&g->mu);
    if (args_type) {
      sd_release(args_type, c->args);
      free(c->args);
    }
    task_free(c);
    return;
  }
  c->next = g->members;
  g->members = c;
  g->running++;
  pthread_mutex_unlock(&g->mu);
  atomic_fetch_add(&sd_running, 1);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, SD_GOROUTINE_STACK);
  int err = pthread_create(&c->thread, &attr, thread_main, c);
  pthread_attr_destroy(&attr);
  if (err) sd_fatal("cannot start a goroutine (out of threads)");
}

/* Waits until every goroutine of g has finished, joins their threads and
 * frees them. If the waiting goroutine is cancelled, so is the group. */
static void group_join(sd_task *t, sd_group *g) {
  pthread_mutex_lock(&t->mu);
  t->waiting_group = g;
  pthread_mutex_unlock(&t->mu);
  if (atomic_load(&t->cancelled)) group_cancel(g);
  reset_wake(t);
  pthread_mutex_lock(&g->mu);
  bool wait = g->running > 0;
  if (wait) g->waiter = t;
  pthread_mutex_unlock(&g->mu);
  if (wait) park(t);
  pthread_mutex_lock(&t->mu);
  t->waiting_group = NULL;
  pthread_mutex_unlock(&t->mu);
  for (sd_task *m = g->members, *next; m; m = next) {
    next = m->next;
    pthread_join(m->thread, NULL);
    task_free(m);
  }
  g->members = NULL;
}

static void group_destroy(sd_group *g) {
  pthread_mutex_destroy(&g->mu);
  free(g);
}

/* Unwinding through an active group: stop its goroutines and wait for them. */
static void group_abort(sd_task *t, sd_group *g) {
  group_cancel(g);
  group_join(t, g);
  sd_err_release(g->err);
  group_destroy(g);
}

sd_err sd_group_wait(sd_group *g) {
  sd_task *t = task_current();
  group_join(t, g);
  sd_err e = g->err;
  for (size_t i = t->ncl; i-- > 0;) {
    if (t->cl[i].group != g) continue;
    if (i == t->ncl - 1) {
      t->arena_len = t->cl[i].off;
      t->ncl--;
    } else {
      t->cl[i].group = NULL; /* tombstone: defers were pushed after it */
    }
    break;
  }
  group_destroy(g);
  return e;
}

/* ------------------------------------------------------------- channels */

sd_chan *sd_chan_new(const sd_type *et, sd_int cap) {
  int64_t n;
  if (!sd_int_to_i64(cap, &n) || n < 0 || n > ((int64_t)1 << 40)) {
    char *s = sd_int_to_cstr(cap);
    sd_crash("invalid channel buffer size: %s", s);
  }
  sd_chan *c = sd_obj_alloc(sizeof *c);
  memset(c, 0, sizeof *c);
  atomic_init(&c->rc, 1);
  pthread_mutex_init(&c->mu, NULL);
  c->et = et;
  c->cap = n;
  c->buf = n ? sd_alloc((size_t)n * et->size) : NULL;
  return c;
}

sd_chan *sd_chan_retain(sd_chan *c) {
  if (c) atomic_fetch_add_explicit(&c->rc, 1, memory_order_relaxed); /* NULL: a null (chan T)? */
  return c;
}

void sd_chan_release(sd_chan *c) {
  if (!c) return;
  if (atomic_fetch_sub_explicit(&c->rc, 1, memory_order_acq_rel) != 1) return;
  if (!c->et->trivial)
    for (int64_t i = 0; i < c->count; i++) sd_release(c->et, c->buf + (size_t)((c->head + i) % c->cap) * c->et->size);
  free(c->buf);
  pthread_mutex_destroy(&c->mu);
  sd_obj_free(c);
}

static void waitq_push(sd_waitq *q, sd_waiter *w) {
  w->next = NULL;
  w->prev = q->tail;
  if (q->tail) q->tail->next = w;
  else q->head = w;
  q->tail = w;
  w->queued = true;
}

static void waitq_remove(sd_waitq *q, sd_waiter *w) {
  if (w->prev) w->prev->next = w->next;
  else q->head = w->next;
  if (w->next) w->next->prev = w->prev;
  else q->tail = w->prev;
  w->queued = false;
}

/* Removes waiters from the head until one can be claimed (its select had not
 * fired or been cancelled yet). */
static sd_waiter *waitq_claim(sd_waitq *q) {
  while (q->head) {
    sd_waiter *w = q->head;
    waitq_remove(q, w);
    int expected = 0;
    if (atomic_compare_exchange_strong(&w->sel->done, &expected, w->idx + 1)) return w;
  }
  return NULL;
}

static void *slot(sd_chan *c, int64_t i) { return c->buf + (size_t)(i % c->cap) * c->et->size; }

/* With c locked: 1 received into dst, 0 closed and empty, -1 would block. */
static int try_recv(sd_chan *c, void *dst) {
  if (c->count > 0) {
    memcpy(dst, slot(c, c->head), c->et->size);
    c->head = (c->head + 1) % c->cap;
    c->count--;
    sd_waiter *w = waitq_claim(&c->sendq);
    if (w) {
      sd_clone_deep(c->et, slot(c, c->head + c->count), w->val);
      c->count++;
      w->result = 1;
      unpark(w->sel->task);
    }
    return 1;
  }
  sd_waiter *w = waitq_claim(&c->sendq);
  if (w) {
    sd_clone_deep(c->et, dst, w->val);
    w->result = 1;
    unpark(w->sel->task);
    return 1;
  }
  return c->closed ? 0 : -1;
}

/* With c locked: 1 sent, 0 closed, -1 would block. */
static int try_send(sd_chan *c, const void *src) {
  if (c->closed) return 0;
  sd_waiter *w = waitq_claim(&c->recvq);
  if (w) {
    sd_clone_deep(c->et, w->val, src);
    w->result = 1;
    unpark(w->sel->task);
    return 1;
  }
  if (c->count < c->cap) {
    sd_clone_deep(c->et, slot(c, c->head + c->count), src);
    c->count++;
    return 1;
  }
  return -1;
}

/* With c locked and not closed: closes it and wakes every waiter. */
static void close_locked(sd_chan *c) {
  c->closed = true;
  sd_waiter *w;
  while ((w = waitq_claim(&c->recvq))) {
    w->result = 0;
    unpark(w->sel->task);
  }
  while ((w = waitq_claim(&c->sendq))) {
    w->result = 0;
    unpark(w->sel->task);
  }
}

void sd_chan_close(sd_chan *c) {
  pthread_mutex_lock(&c->mu);
  if (c->closed) {
    pthread_mutex_unlock(&c->mu);
    sd_crash("close of closed channel");
  }
  close_locked(c);
  pthread_mutex_unlock(&c->mu);
}

static int sort_unique_chans(sd_chan **cs, int n) {
  for (int i = 1; i < n; i++) {
    sd_chan *x = cs[i];
    int j = i;
    while (j > 0 && (uintptr_t)cs[j - 1] > (uintptr_t)x) {
      cs[j] = cs[j - 1];
      j--;
    }
    cs[j] = x;
  }
  int m = 0;
  for (int i = 0; i < n; i++)
    if (m == 0 || cs[m - 1] != cs[i]) cs[m++] = cs[i];
  return m;
}

static void lock_all(sd_chan **cs, int n) {
  for (int i = 0; i < n; i++) pthread_mutex_lock(&cs[i]->mu);
}
static void unlock_all(sd_chan **cs, int n) {
  for (int i = n; i-- > 0;) pthread_mutex_unlock(&cs[i]->mu);
}

#define SD_SELECT_INLINE 8

int sd_select(sd_select_case *cases, int n) {
  sd_task *t = task_current();
  check_cancel(t);
  int order_buf[SD_SELECT_INLINE];
  sd_chan *chans_buf[SD_SELECT_INLINE];
  sd_waiter ws_buf[SD_SELECT_INLINE];
  int *order = n <= SD_SELECT_INLINE ? order_buf : sd_alloc((size_t)n * sizeof(int));
  sd_chan **chans = n <= SD_SELECT_INLINE ? chans_buf : sd_alloc((size_t)n * sizeof(sd_chan *));
  sd_waiter *ws = n <= SD_SELECT_INLINE ? ws_buf : sd_alloc((size_t)n * sizeof(sd_waiter));
  for (int i = 0; i < n; i++) {
    order[i] = i;
    chans[i] = cases[i].ch;
  }
  for (int i = n - 1; i > 0; i--) { /* random poll order: fair choice */
    int j = (int)(sd_rand(t) % (uint64_t)(i + 1));
    int x = order[i];
    order[i] = order[j];
    order[j] = x;
  }
  int nc = sort_unique_chans(chans, n);
  int chosen = -1;
  bool send_closed = false;

  lock_all(chans, nc);
  for (int k = 0; k < n && chosen < 0; k++) {
    int i = order[k];
    sd_select_case *sc = &cases[i];
    if (sc->dir == SD_RECV) {
      int r = try_recv(sc->ch, sc->val);
      if (r >= 0) {
        sc->ok = r == 1;
        chosen = i;
      }
    } else {
      int r = try_send(sc->ch, sc->val);
      if (r >= 0) {
        chosen = i;
        send_closed = r == 0;
      }
    }
  }
  if (chosen < 0) {
    sd_sel sel;
    atomic_init(&sel.done, 0);
    sel.task = t;
    for (int i = 0; i < n; i++) {
      ws[i] = (sd_waiter){.sel = &sel, .idx = i, .val = cases[i].val};
      waitq_push(cases[i].dir == SD_RECV ? &cases[i].ch->recvq : &cases[i].ch->sendq, &ws[i]);
    }
    pthread_mutex_lock(&t->mu);
    t->sel = &sel;
    t->wake = false;
    pthread_mutex_unlock(&t->mu);
    unlock_all(chans, nc);

    bool self_cancelled = false;
    if (atomic_load(&t->cancelled)) {
      int expected = 0;
      self_cancelled = atomic_compare_exchange_strong(&sel.done, &expected, SEL_CANCELLED);
    }
    if (!self_cancelled) park(t);
    pthread_mutex_lock(&t->mu);
    t->sel = NULL;
    pthread_mutex_unlock(&t->mu);

    lock_all(chans, nc);
    for (int i = 0; i < n; i++)
      if (ws[i].queued) waitq_remove(cases[i].dir == SD_RECV ? &cases[i].ch->recvq : &cases[i].ch->sendq, &ws[i]);
    int d = atomic_load(&sel.done);
    if (d != SEL_CANCELLED) {
      chosen = d - 1;
      if (cases[chosen].dir == SD_RECV) cases[chosen].ok = ws[chosen].result == 1;
      else send_closed = ws[chosen].result == 0;
    }
  }
  unlock_all(chans, nc);
  if (n > SD_SELECT_INLINE) {
    free(order);
    free(chans);
    free(ws);
  }
  if (chosen < 0) stop_task(t);
  if (send_closed) sd_crash("send on closed channel");
  return chosen;
}

void sd_chan_send(sd_chan *c, const void *val) {
  sd_select_case sc = {c, SD_SEND, (void *)val, false};
  sd_select(&sc, 1);
}

bool sd_chan_receive(sd_chan *c, void *out) {
  sd_select_case sc = {c, SD_RECV, out, false};
  sd_select(&sc, 1);
  return sc.ok;
}

/* ---------------------------------------------------------------- time */

static int64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

sd_int sd_time_now(void) { return sd_int_from_i64(mono_ns()); }

typedef struct {
  int64_t deadline;
  uint64_t seq;
  sd_chan *c;
} sd_timer;

static pthread_mutex_t timer_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timer_cv = PTHREAD_COND_INITIALIZER;
static sd_timer *timers;
static size_t ntimers, captimers;
static uint64_t timer_seq;
static bool timer_started;

static bool timer_before(const sd_timer *a, const sd_timer *b) {
  return a->deadline < b->deadline || (a->deadline == b->deadline && a->seq < b->seq);
}

static void timer_push(sd_timer x) {
  if (ntimers == captimers) {
    captimers = captimers ? captimers * 2 : 16;
    timers = sd_realloc(timers, captimers * sizeof *timers);
  }
  size_t i = ntimers++;
  while (i > 0 && timer_before(&x, &timers[(i - 1) / 2])) {
    timers[i] = timers[(i - 1) / 2];
    i = (i - 1) / 2;
  }
  timers[i] = x;
}

static sd_timer timer_pop(void) {
  sd_timer top = timers[0], x = timers[--ntimers];
  size_t i = 0;
  for (;;) {
    size_t l = 2 * i + 1, r = l + 1, m = i;
    const sd_timer *best = &x;
    if (l < ntimers && timer_before(&timers[l], best)) {
      m = l;
      best = &timers[l];
    }
    if (r < ntimers && timer_before(&timers[r], best)) m = r;
    if (m == i) break;
    timers[i] = timers[m];
    i = m;
  }
  if (ntimers) timers[i] = x;
  return top;
}

static void *timer_main(void *p) {
  (void)p;
  pthread_mutex_lock(&timer_mu);
  for (;;) {
    if (ntimers == 0) {
      pthread_cond_wait(&timer_cv, &timer_mu);
      continue;
    }
    int64_t now = mono_ns();
    if (timers[0].deadline > now) {
      int64_t delta = timers[0].deadline - now;
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      int64_t ns = ts.tv_nsec + delta % 1000000000;
      ts.tv_sec += (time_t)(delta / 1000000000 + ns / 1000000000);
      ts.tv_nsec = (long)(ns % 1000000000);
      pthread_cond_timedwait(&timer_cv, &timer_mu, &ts);
      continue;
    }
    sd_timer x = timer_pop();
    pthread_mutex_unlock(&timer_mu);
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    sd_int v = sd_int_from_i64((int64_t)wall.tv_sec * 1000000000 + wall.tv_nsec);
    pthread_mutex_lock(&x.c->mu);
    try_send(x.c, &v); /* buffer of one, sent once: never blocks */
    close_locked(x.c); /* later receives return null instead of waiting forever */
    pthread_mutex_unlock(&x.c->mu);
    sd_chan_release(x.c);
    running_dec();
    pthread_mutex_lock(&timer_mu);
  }
  return NULL;
}

sd_chan *sd_time_after(sd_int ns) {
  int64_t d;
  if (!sd_int_to_i64(ns, &d) || d > INT64_MAX / 4) d = INT64_MAX / 4;
  if (d < 0) d = 0;
  sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(1));
  atomic_fetch_add(&sd_running, 1); /* a pending timer can still wake someone */
  pthread_mutex_lock(&timer_mu);
  timer_push((sd_timer){mono_ns() + d, timer_seq++, sd_chan_retain(c)});
  if (!timer_started) {
    timer_started = true;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, timer_main, NULL) != 0) sd_fatal("cannot start the timer thread");
    pthread_attr_destroy(&attr);
  }
  pthread_cond_signal(&timer_cv);
  pthread_mutex_unlock(&timer_mu);
  return c;
}

void sd_time_sleep(sd_int ns) {
  sd_chan *c = sd_time_after(ns);
  sd_int v;
  if (sd_chan_receive(c, &v)) sd_int_release(v);
  sd_chan_release(c);
}

/* ------------------------------------------------------- program entry */

void sd_exit_process(int code) {
  fflush(stdout);
  fflush(stderr);
  _exit(code);
}

#if !defined(SD_NO_FAULT_HANDLER)
static void fault_handler(int sig) {
  (void)sig;
  sd_task *t = sd_cur;
  if (t && t->frame) {
    if (t->raise == SD_RAISE_NONE) {
      t->raise = SD_RAISE_CRASH;
      t->msg = (char *)"stack overflow or invalid memory access";
      t->msg_static = true;
    }
    siglongjmp(t->frame->jb, 1);
  }
  static const char m[] = "crash: stack overflow or invalid memory access\n";
  if (write(2, m, sizeof m - 1) < 0) _exit(2);
  _exit(2);
}
#endif

static void install_fault_handler(void) {
#if !defined(SD_NO_FAULT_HANDLER)
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fault_handler;
  sa.sa_flags = SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sd_fault_installed = true;
#endif
}

typedef struct {
  sd_err (*fn)(void *);
  void *arg;
  sd_err result;
} catch_call;

static void run_catch(void *p) {
  catch_call *cc = p;
  cc->result = cc->fn(cc->arg);
}

sd_err sd_catch(sd_err (*fn)(void *arg), void *arg) {
  sd_task *t = task_current();
  catch_call cc = {fn, arg, SD_ERR_NULL};
  int r = protect(t, run_catch, &cc);
  if (r == SD_RAISE_NONE) return cc.result;
  if (r == SD_RAISE_STOP) {
    t->raise = SD_RAISE_NONE;
    stop_task(t); /* cancellation goes on to the goroutine's top */
  }
  char *msg;
  take_raise(t, &msg);
  sd_err e = sd_errorf("crash: %s", msg);
  free(msg);
  return e;
}

typedef struct {
  sd_err (*fn)(sd_list *);
  sd_list *args;
  sd_err result;
} main_call;

static void run_main(void *p) {
  main_call *mc = p;
  mc->result = mc->fn(mc->args);
}

static char sd_stdout_buf[1 << 16];

int sd_main(int argc, char **argv, sd_err (*main_fn)(sd_list *args)) {
  setvbuf(stdout, sd_stdout_buf, _IOFBF, sizeof sd_stdout_buf);
  install_fault_handler();
  sd_task *t = task_current();
  setup_altstack(t);
  sd_list *args = sd_list_new(&sd_type_str, argc);
  for (int i = 0; i < argc; i++) SD_LIST_PUSH(&args, sd_str, sd_str_from_cstr(argv[i]));
  main_call mc = {main_fn, args, SD_ERR_NULL};
  int r = protect(t, run_main, &mc);
  if (r != SD_RAISE_NONE) {
    char *msg;
    take_raise(t, &msg);
    fflush(stdout);
    fprintf(stderr, "crash: %s\n", msg);
    sd_exit_process(2);
  }
  sd_list_release(args);
  if (!sd_err_is_null(mc.result)) {
    sd_str m = sd_err_message(mc.result);
    fflush(stdout);
    fputs("error: ", stderr);
    fwrite(m->data, 1, (size_t)m->len, stderr);
    fputc('\n', stderr);
    sd_exit_process(1);
  }
  sd_exit_process(0);
}
