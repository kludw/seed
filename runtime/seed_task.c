/* Goroutines (one OS thread each), crashes and defer, groups, channels,
 * select, timers, blocking calls and the handle table, and program entry.
 *
 * Locking order (never acquire against it):
 *   channel locks (several at once: in address order)
 *     -> task->mu
 *   group->mu -> task->mu -> (that task's waiting group)->mu -> ...  (down the tree)
 *   handle->mu -> task->mu (close waking a waiter)
 *   timer lock and the handle table's lock are never held while taking another lock.
 */
#include "seed_internal.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

  pthread_mutex_t mu; /* protects wake, sleeping, sel, waiting_group, blocking */
  pthread_cond_t cv;
  bool wake, sleeping;
  sd_sel *sel;
  sd_group *waiting_group;
  atomic_int cancelled;
  /* Blocking calls: the goroutine's wake-up pipe (created on first use; -1
   * before) and whether it is waiting in poll() on it now. */
  int wake_r, wake_w;
  bool blocking;

  sd_group *group;          /* the group this goroutine belongs to (NULL for main) */
  sd_task *next, *prev;     /* the other running members of that group */
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
  sd_task *members; /* running goroutines */
  sd_task *dead;    /* finished goroutines not joined yet (linked by next) */
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
  t->wake_r = t->wake_w = -1;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  t->rng = ((uint64_t)ts.tv_nsec * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)(uintptr_t)t << 7) ^
           (atomic_fetch_add(&sd_seed_counter, 1) * 0xBF58476D1CE4E5B9ULL);
  if (!t->rng) t->rng = 0x853c49e6748fea9bULL;
  return t;
}

static void task_free(sd_task *t) {
  if (t->wake_r >= 0) {
    close(t->wake_r);
    close(t->wake_w);
  }
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

static void task_wake_locked(sd_task *m);

static void task_cancel(sd_task *m) {
  atomic_store(&m->cancelled, 1);
  pthread_mutex_lock(&m->mu);
  if (m->blocking) task_wake_locked(m);
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
  /* Move to the dead list: the next spawn in the group (or the group's wait)
   * joins the thread and frees the task, so a long-lived group (a server)
   * doesn't keep every finished goroutine. */
  if (t->prev) t->prev->next = t->next;
  else g->members = t->next;
  if (t->next) t->next->prev = t->prev;
  t->prev = NULL;
  t->next = g->dead;
  g->dead = t;
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

/* Joins and frees a list of finished goroutines (linked by next). */
static void reap(sd_task *m) {
  for (sd_task *next; m; m = next) {
    next = m->next;
    pthread_join(m->thread, NULL);
    task_free(m);
  }
}

void sd_spawn(sd_group *g, sd_task_fn body, const sd_type *args_type, const void *args) {
  sd_task *parent = task_current();
  pthread_mutex_lock(&g->mu);
  sd_task *dead = g->dead;
  g->dead = NULL;
  pthread_mutex_unlock(&g->mu);
  reap(dead);
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
  if (c->next) c->next->prev = c;
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
  /* Every member has finished, so all are on the dead list now. */
  pthread_mutex_lock(&g->mu);
  sd_task *dead = g->dead;
  g->dead = NULL;
  pthread_mutex_unlock(&g->mu);
  reap(dead);
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

/* ------------------------------------------------------- blocking calls */

/* A native that waits for the OS (a socket, stdin, a child process) waits in
 * poll() on its descriptor and on the goroutine's wake-up pipe. Cancelling
 * the goroutine (task_cancel) writes a byte to the pipe while it waits, so
 * the wait returns SD_WAIT_STOP and the native returns into the stop path
 * (sd_stop). Closing a handle wakes the goroutine waiting on it the same
 * way. A goroutine waiting in poll() counts as running for the deadlock
 * check: something outside the program can still wake it. */

#define SD_WAIT_WOKEN (-3) /* internal: woken without a stop request */
#define SD_WAIT_SLICE_MS 50 /* without a wake-up pipe, check for a stop this often */

static void fd_set_flags(int fd, bool nonblock) {
  fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  if (nonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* With t->mu held, on t's own thread: creates the wake-up pipe if needed.
 * If no descriptors are left, waits poll in short slices instead. */
static void wake_pipe_locked(sd_task *t) {
  if (t->wake_r >= 0) return;
  int p[2];
  if (pipe(p) != 0) return;
  fd_set_flags(p[0], true);
  fd_set_flags(p[1], true);
  t->wake_r = p[0];
  t->wake_w = p[1];
}

/* With m->mu held: wakes m if it waits in poll() (a full pipe already has a
 * wake-up pending). */
static void task_wake_locked(sd_task *m) {
  if (m->wake_w < 0) return;
  char c = 1;
  ssize_t r = write(m->wake_w, &c, 1);
  (void)r;
}

/* Waits for pf[0..n) (n <= 1) until deadline (monotonic ns; 0: none).
 * Returns SD_WAIT_READY (with revents set), SD_WAIT_TIMEOUT, SD_WAIT_STOP,
 * SD_WAIT_FAILED (errno set) or, when wakeable, SD_WAIT_WOKEN. */
static int wait_core(sd_task *t, struct pollfd *pf, int n, int64_t deadline, bool wakeable) {
  struct pollfd all[2];
  pthread_mutex_lock(&t->mu);
  wake_pipe_locked(t);
  t->blocking = true;
  pthread_mutex_unlock(&t->mu);
  int wfd = t->wake_r, result, saved = 0;
  for (;;) {
    if (atomic_load(&t->cancelled)) {
      result = SD_WAIT_STOP;
      break;
    }
    int ms = -1;
    if (deadline) {
      int64_t left = deadline - mono_ns();
      if (left <= 0) {
        result = SD_WAIT_TIMEOUT;
        break;
      }
      int64_t lms = (left + 999999) / 1000000;
      ms = lms > INT_MAX ? INT_MAX : (int)lms;
    }
    if (wfd < 0 && (ms < 0 || ms > SD_WAIT_SLICE_MS)) ms = SD_WAIT_SLICE_MS;
    for (int i = 0; i < n; i++) {
      all[i] = pf[i];
      all[i].revents = 0;
    }
    int m = n;
    if (wfd >= 0) all[m++] = (struct pollfd){wfd, POLLIN, 0};
    int r = poll(all, (nfds_t)m, ms);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      saved = errno;
      result = SD_WAIT_FAILED;
      break;
    }
    bool woken = wfd >= 0 && all[n].revents != 0, ready = false;
    if (woken) {
      char buf[64];
      while (read(wfd, buf, sizeof buf) > 0) {
      }
    }
    for (int i = 0; i < n; i++) {
      pf[i].revents = all[i].revents;
      if (all[i].revents) ready = true;
    }
    if (atomic_load(&t->cancelled)) {
      result = SD_WAIT_STOP;
      break;
    }
    if (ready) {
      result = SD_WAIT_READY;
      break;
    }
    if (woken && wakeable) {
      result = SD_WAIT_WOKEN;
      break;
    }
  }
  pthread_mutex_lock(&t->mu);
  t->blocking = false;
  pthread_mutex_unlock(&t->mu);
  if (result == SD_WAIT_FAILED) errno = saved;
  return result;
}

int sd_wait_fd(int fd, bool for_write, int64_t timeout_ns) {
  struct pollfd pf = {fd, for_write ? POLLOUT : POLLIN, 0};
  return wait_core(task_current(), &pf, 1, timeout_ns > 0 ? mono_ns() + timeout_ns : 0, false);
}

bool sd_stopping(void) { return atomic_load(&task_current()->cancelled) != 0; }

void sd_stop(void) { stop_task(task_current()); }

typedef struct {
  pid_t pid;
  int status, err, fd;
} child_wait;

/* A helper thread does the blocking waitpid, then signals the pipe that the
 * goroutine polls (POSIX has no descriptor for a child process). */
static void *child_waiter(void *p) {
  child_wait *cw = p;
  for (;;) {
    if (waitpid(cw->pid, &cw->status, 0) >= 0) break;
    if (errno != EINTR) {
      cw->err = errno;
      break;
    }
  }
  char c = 1;
  ssize_t r = write(cw->fd, &c, 1);
  (void)r;
  return NULL;
}

int sd_wait_child(pid_t pid, int *status) {
  child_wait cw = {pid, 0, 0, -1};
  int p[2];
  pthread_t th;
  if (pipe(p) == 0) {
    fd_set_flags(p[0], false);
    fd_set_flags(p[1], false);
    cw.fd = p[1];
    if (pthread_create(&th, NULL, child_waiter, &cw) == 0) {
      int r;
      do r = sd_wait_fd(p[0], false, 0);
      while (r == SD_WAIT_FAILED);
      if (r == SD_WAIT_STOP) kill(pid, SIGKILL);
      pthread_join(th, NULL);
      close(p[0]);
      close(p[1]);
      if (r == SD_WAIT_STOP) return SD_WAIT_STOP;
      *status = cw.status;
      return cw.err;
    }
    close(p[0]);
    close(p[1]);
  }
  /* No pipe or thread: wait without being a stop point. */
  while (waitpid(pid, status, 0) < 0)
    if (errno != EINTR) return errno;
  return 0;
}

/* -------------------------------------------------------------- handles */

#ifdef MSG_NOSIGNAL
#define SD_MSG_NOSIGNAL MSG_NOSIGNAL
#else
#define SD_MSG_NOSIGNAL 0 /* macOS: the socket has SO_NOSIGPIPE instead */
#endif

struct sd_handle {
  int64_t id;
  int kind, fd;
  bool nonblock; /* fd is O_NONBLOCK: try the call first, poll on EAGAIN */
  char *name;
  atomic_int refs; /* the table's, plus one per operation in progress */
  sd_handle *hnext;
  pthread_mutex_t mu; /* protects closed, busy, waiter, timeout */
  pthread_cond_t cv;
  bool closed, busy;
  sd_task *waiter; /* the goroutine in poll() on fd, woken by close */
  int64_t timeout;
  /* The read buffer (used under the operation lock): bytes [pos, end) are
   * unread; [pos, pos + scanned) have no newline. */
  char *buf;
  size_t pos, end, cap, scanned;
};

static pthread_mutex_t handles_mu = PTHREAD_MUTEX_INITIALIZER;
static sd_handle **handles; /* hash table by id, chained; capacity a power of two */
static size_t handles_cap, handles_len;
static int64_t handles_next = 1;

static void handle_unref(sd_handle *h) {
  if (atomic_fetch_sub(&h->refs, 1) != 1) return;
  if (h->fd >= 0 && h->kind != SD_HANDLE_STDIN) close(h->fd);
  pthread_mutex_destroy(&h->mu);
  pthread_cond_destroy(&h->cv);
  free(h->name);
  free(h->buf);
  free(h);
}

int64_t sd_handle_new(int kind, int fd, const char *name) {
  sd_handle *h = calloc(1, sizeof *h);
  if (!h) sd_fatal("out of memory");
  h->kind = kind;
  h->fd = fd;
  int fl = fcntl(fd, F_GETFL);
  h->nonblock = fl >= 0 && (fl & O_NONBLOCK);
  h->name = sd_fmt("%s", name ? name : "");
  atomic_init(&h->refs, 1);
  pthread_mutex_init(&h->mu, NULL);
  pthread_cond_init(&h->cv, NULL);
  pthread_mutex_lock(&handles_mu);
  if (handles_len + 1 > handles_cap / 2) {
    size_t cap = handles_cap ? handles_cap * 2 : 64;
    sd_handle **t = calloc(cap, sizeof *t);
    if (!t) sd_fatal("out of memory");
    for (size_t i = 0; i < handles_cap; i++)
      for (sd_handle *e = handles[i], *next; e; e = next) {
        next = e->hnext;
        e->hnext = t[(size_t)e->id & (cap - 1)];
        t[(size_t)e->id & (cap - 1)] = e;
      }
    free(handles);
    handles = t;
    handles_cap = cap;
  }
  h->id = handles_next++;
  size_t b = (size_t)h->id & (handles_cap - 1);
  h->hnext = handles[b];
  handles[b] = h;
  handles_len++;
  int64_t id = h->id;
  pthread_mutex_unlock(&handles_mu);
  return id;
}

/* The handle with a reference added, or NULL. With remove, it is taken out
 * of the table instead, and the caller gets the table's reference. */
static sd_handle *handle_find(int64_t id, bool remove) {
  sd_handle *found = NULL;
  pthread_mutex_lock(&handles_mu);
  if (handles_cap) {
    sd_handle **p = &handles[(size_t)id & (handles_cap - 1)];
    for (; *p; p = &(*p)->hnext) {
      if ((*p)->id != id) continue;
      found = *p;
      if (remove) {
        *p = found->hnext;
        handles_len--;
      } else {
        atomic_fetch_add(&found->refs, 1);
      }
      break;
    }
  }
  pthread_mutex_unlock(&handles_mu);
  return found;
}

sd_handle *sd_handle_acquire(int64_t id, int kind) {
  sd_handle *h = handle_find(id, false);
  if (!h) return NULL;
  if (h->kind != kind) {
    handle_unref(h);
    return NULL;
  }
  sd_task *t = task_current();
  pthread_mutex_lock(&h->mu);
  while (h->busy && !h->closed) {
    /* Another goroutine's operation on this handle: wait for it, checking
     * for a stop request now and then. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 20 * 1000000;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&h->cv, &h->mu, &ts);
    if (atomic_load(&t->cancelled)) {
      pthread_mutex_unlock(&h->mu);
      handle_unref(h);
      stop_task(t);
    }
  }
  if (h->closed) {
    pthread_mutex_unlock(&h->mu);
    handle_unref(h);
    return NULL;
  }
  h->busy = true;
  pthread_mutex_unlock(&h->mu);
  return h;
}

void sd_handle_release(sd_handle *h) {
  pthread_mutex_lock(&h->mu);
  h->busy = false;
  pthread_cond_signal(&h->cv);
  pthread_mutex_unlock(&h->mu);
  handle_unref(h);
}

bool sd_handle_close(int64_t id) {
  sd_handle *h = handle_find(id, true);
  if (!h) return false;
  pthread_mutex_lock(&h->mu);
  h->closed = true;
  if (h->waiter) {
    pthread_mutex_lock(&h->waiter->mu);
    task_wake_locked(h->waiter);
    pthread_mutex_unlock(&h->waiter->mu);
  }
  pthread_cond_broadcast(&h->cv);
  pthread_mutex_unlock(&h->mu);
  handle_unref(h); /* the table's: the descriptor is closed once no operation uses it */
  return true;
}

void sd_handle_set_timeout(int64_t id, int64_t ns) {
  sd_handle *h = handle_find(id, false);
  if (!h) return;
  pthread_mutex_lock(&h->mu);
  h->timeout = ns > 0 ? ns : 0;
  pthread_mutex_unlock(&h->mu);
  handle_unref(h);
}

sd_str sd_handle_subject(int64_t id) {
  sd_handle *h = handle_find(id, false);
  if (!h) return sd_str_from_cstr("");
  sd_str s = sd_str_from_cstr(h->name); /* the name never changes */
  handle_unref(h);
  return s;
}

int sd_handle_fd(const sd_handle *h) { return h->fd; }

const char *sd_handle_name(const sd_handle *h) { return h->name; }

static int handle_wait(sd_handle *h, bool for_write, int *err) {
  sd_task *t = task_current();
  pthread_mutex_lock(&h->mu);
  int64_t deadline = h->timeout > 0 ? mono_ns() + h->timeout : 0;
  pthread_mutex_unlock(&h->mu);
  for (;;) {
    pthread_mutex_lock(&h->mu);
    if (h->closed) {
      pthread_mutex_unlock(&h->mu);
      return SD_IO_CLOSED;
    }
    h->waiter = t;
    pthread_mutex_unlock(&h->mu);
    struct pollfd pf = {h->fd, for_write ? POLLOUT : POLLIN, 0};
    int r = wait_core(t, &pf, 1, deadline, true);
    int saved = errno;
    pthread_mutex_lock(&h->mu);
    h->waiter = NULL;
    bool closed = h->closed;
    pthread_mutex_unlock(&h->mu);
    if (r == SD_WAIT_STOP) {
      sd_handle_release(h);
      stop_task(t);
    }
    if (closed) return SD_IO_CLOSED;
    switch (r) {
    case SD_WAIT_READY: return SD_IO_OK;
    case SD_WAIT_TIMEOUT: return SD_IO_TIMEOUT;
    case SD_WAIT_FAILED:
      if (err) *err = saved;
      return SD_IO_ERROR;
    default: break; /* woken by something else: look again */
    }
  }
}

int sd_handle_wait(sd_handle *h, bool for_write) {
  int err = 0;
  int r = handle_wait(h, for_write, &err);
  if (r == SD_IO_ERROR) errno = err;
  return r;
}

/* One read() of at most n bytes into dst, waiting as needed. */
static int handle_read_some(sd_handle *h, char *dst, size_t n, size_t *got, int *err) {
  for (;;) {
    bool waited = false;
    if (!h->nonblock) {
      /* A blocking descriptor (stdin): read() only after poll() says there
       * is something, so that the wait stays a stop point. */
      int w = handle_wait(h, false, err);
      if (w != SD_IO_OK) return w;
      waited = true;
    }
    ssize_t r = read(h->fd, dst, n);
    if (r > 0) {
      *got = (size_t)r;
      return SD_IO_OK;
    }
    if (r == 0) return SD_IO_EOF;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      *err = errno;
      return SD_IO_ERROR;
    }
    if (!waited) {
      int w = handle_wait(h, false, err);
      if (w != SD_IO_OK) return w;
    }
  }
}

/* Reads more bytes into the buffer. */
static int handle_fill(sd_handle *h, int *err) {
  if (h->cap - h->end < 4096) {
    if (h->pos > 0) {
      memmove(h->buf, h->buf + h->pos, h->end - h->pos);
      h->end -= h->pos;
      h->pos = 0;
    }
    if (h->cap - h->end < 4096) {
      h->cap = h->cap ? h->cap * 2 : 65536;
      h->buf = sd_realloc(h->buf, h->cap);
    }
  }
  size_t got = 0;
  int r = handle_read_some(h, h->buf + h->end, h->cap - h->end, &got, err);
  if (r == SD_IO_OK) h->end += got;
  return r;
}

/* Takes n buffered bytes and skips `skip` more. */
static sd_str handle_take(sd_handle *h, size_t n, size_t skip) {
  sd_str s = sd_str_new(h->buf + h->pos, (int64_t)n);
  h->pos += n + skip;
  h->scanned = 0;
  if (h->pos == h->end) h->pos = h->end = 0;
  return s;
}

int sd_handle_read(sd_handle *h, int64_t max, sd_str *out, int *err) {
  *out = NULL;
  if (max < 1) max = 1;
  if (h->end > h->pos) {
    size_t n = h->end - h->pos;
    if ((uint64_t)max < n) n = (size_t)max;
    *out = handle_take(h, n, 0);
    return SD_IO_OK;
  }
  char tmp[65536];
  size_t n = (uint64_t)max < sizeof tmp ? (size_t)max : sizeof tmp, got = 0;
  int r = handle_read_some(h, tmp, n, &got, err);
  if (r == SD_IO_OK) *out = sd_str_new(tmp, (int64_t)got);
  return r;
}

int sd_handle_read_line(sd_handle *h, sd_str *out, int *err) {
  *out = NULL;
  for (;;) {
    size_t from = h->pos + h->scanned;
    char *nl = h->end > from ? memchr(h->buf + from, '\n', h->end - from) : NULL;
    if (nl) {
      size_t n = (size_t)(nl - (h->buf + h->pos));
      size_t cr = n > 0 && h->buf[h->pos + n - 1] == '\r';
      *out = handle_take(h, n - cr, 1 + cr);
      return SD_IO_OK;
    }
    h->scanned = h->end - h->pos;
    int r = handle_fill(h, err); /* scanned is relative to pos, so it stays valid */
    if (r == SD_IO_EOF) {
      if (h->end == h->pos) return SD_IO_EOF;
      *out = handle_take(h, h->end - h->pos, 0);
      return SD_IO_OK;
    }
    if (r != SD_IO_OK) return r;
  }
}

int sd_handle_read_all(sd_handle *h, sd_str *out, int *err) {
  *out = NULL;
  for (;;) {
    int r = handle_fill(h, err);
    if (r == SD_IO_EOF) break;
    if (r != SD_IO_OK) return r;
  }
  *out = handle_take(h, h->end - h->pos, 0);
  return SD_IO_OK;
}

int sd_handle_write(sd_handle *h, const char *data, int64_t len, int *err) {
  size_t off = 0, n = (size_t)len;
  while (off < n) {
    ssize_t r = h->kind == SD_HANDLE_CONN ? send(h->fd, data + off, n - off, SD_MSG_NOSIGNAL) : write(h->fd, data + off, n - off);
    if (r > 0) {
      off += (size_t)r;
      continue;
    }
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      *err = errno;
      return SD_IO_ERROR;
    }
    int w = handle_wait(h, true, err);
    if (w != SD_IO_OK) return w;
  }
  return SD_IO_OK;
}

static const char *handle_closed_text(int kind) {
  switch (kind) {
  case SD_HANDLE_FILE: return "file is closed";
  case SD_HANDLE_LISTENER: return "listener is closed";
  case SD_HANDLE_STDIN: return "stdin is closed";
  default: return "connection is closed";
  }
}

sd_err sd_handle_error(const char *fn, const sd_handle *h, int kind, int result, int err) {
  const char *reason;
  switch (result) {
  case SD_IO_TIMEOUT: reason = "timeout"; break;
  case SD_IO_CLOSED: reason = handle_closed_text(kind); break;
  case SD_IO_EOF: reason = "unexpected end of input"; break;
  default: reason = sd_strerror(err); break;
  }
  if (h && h->name[0]) return sd_errorf("%s: %s: %s", fn, h->name, reason);
  return sd_errorf("%s: %s", fn, reason);
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
