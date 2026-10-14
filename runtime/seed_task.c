/* Goroutines and their scheduler, crashes and defer, groups, channels,
 * select, timers and the poller, blocking calls and the handle table, and
 * program entry.
 *
 * Goroutines are scheduled M:N (runtime/README.md, section 10). Each one runs
 * on a stack of its own (mmap'd, with a guard region below it) and is
 * switched in user space (sd_ctx_swap_) onto worker threads. As in Go's
 * runtime: a G is a goroutine (sd_task), an M a worker thread (sd_m), a P a
 * processor (sd_p): one per CPU, each with a run queue. An M runs goroutines
 * while it holds a P; an M without work steals from the other Ps, then gives
 * its P back and sleeps. A goroutine that waits (a channel, a group, a
 * descriptor, a timer) parks: it switches to its M's own stack (the
 * scheduler context, "g0"), which runs the next goroutine. Descriptors and
 * timers are watched by one poller thread (epoll or kqueue); calls that block
 * in the OS run in a "block section" (sd_block_begin/end), whose P the
 * sysmon thread hands to another M when the call takes long.
 *
 * Thread-local storage: a goroutine can move to another thread at any call
 * that may park, so per-goroutine state lives in sd_task, and the thread's
 * own variables are only read through the noinline accessors below (see
 * "Thread-local storage" in runtime/README.md).
 *
 * Locking order (never acquire against it):
 *   channel locks (several at once: in address order)
 *     -> task->mu
 *   group->mu -> task->mu -> (that task's waiting group)->mu -> ...  (down the tree)
 *   handle->mu -> task->mu (close waking a waiter)
 *   timer_mu, poll_mu: never held while taking another of the locks above.
 * unpark (readying a goroutine) takes only sched.mu, an M's mu and a
 * foreign task's pmu, which are taken last, so it may be called anywhere.
 */
#include "seed_internal.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#define SD_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/event.h>
#define SD_KQUEUE 1
#endif

#if !defined(SD_CTX_UCONTEXT) && !defined(__x86_64__) && !defined(__aarch64__)
#define SD_CTX_UCONTEXT 1
#endif
#if defined(SD_CTX_UCONTEXT)
#include <ucontext.h>
#endif

/* Stack size of goroutines; the main goroutine's is the stack limit
 * (ulimit -s) like a main thread's. Reserved, not committed: pages are used
 * as the stack grows. */
#ifndef SD_GOROUTINE_STACK
#define SD_GOROUTINE_STACK ((size_t)8 << 20)
#endif
#define SD_STACK_GUARD ((size_t)64 << 10)  /* inaccessible, below each stack */
#define SD_STACK_KEEP ((size_t)64 << 10)   /* a pooled stack keeps this much of its top */
#define SD_SLAB_STACKS 16                  /* unguarded stacks per mapping */
#define SD_MAX_PROCS 256
#define SD_RUNQ 256                        /* a P's run queue (a power of two) */
#define SD_GFREE_LOCAL 64                  /* finished goroutines kept per P ... */
#define SD_GFREE_GLOBAL 4096               /* ... and shared */

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define SD_NO_FAULT_HANDLER 1
#endif
#if __has_feature(address_sanitizer)
#define SD_ASAN 1
#endif
#if __has_feature(thread_sanitizer)
#define SD_TSAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define SD_NO_FAULT_HANDLER 1
#ifndef SD_ASAN
#define SD_ASAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define SD_NO_FAULT_HANDLER 1
#ifndef SD_TSAN
#define SD_TSAN 1
#endif
#endif

/* The sanitizers' fiber interfaces: they must be told about every switch
 * between stacks. */
#if defined(SD_ASAN)
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old, size_t *size_old);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#endif
#if defined(SD_TSAN)
void *__tsan_get_current_fiber(void);
void *__tsan_create_fiber(unsigned flags);
void __tsan_destroy_fiber(void *fiber);
void __tsan_switch_to_fiber(void *fiber, unsigned flags);
#endif

typedef struct sd_task sd_task;
typedef struct sd_sel sd_sel;
typedef struct sd_m sd_m;
typedef struct sd_p sd_p;

/* ------------------------------------------------------------- structures */

typedef struct sd_frame {
  sigjmp_buf jb;
  struct sd_frame *prev;
  size_t mark;       /* cleanup-stack depth when the frame was entered */
  sd_lframe *lmark;  /* top of the cleanup-frame chain when it was entered */
} sd_frame;

/* One entry of a goroutine's cleanup stack: a deferred call (fn != NULL), an
 * active group (group != NULL), or a tombstone (both NULL). */
typedef struct sd_cleanup {
  void (*fn)(void *env);
  sd_group *group;
  size_t off, size; /* env bytes in the arena */
} sd_cleanup;

enum { SD_RAISE_NONE = 0, SD_RAISE_CRASH, SD_RAISE_STOP };

/* A timer: time.after's channel (c), or the deadline of a goroutine's wait
 * (task). In the heap while idx != SD_TIMER_OFF. */
typedef struct sd_timer {
  int64_t deadline;
  uint64_t seq;
  size_t idx;
  sd_chan *c;
  sd_task *task;
} sd_timer;
#define SD_TIMER_OFF SIZE_MAX

typedef struct {
  char *map;       /* the mapping (guarded: the guard and the stack) */
  size_t maplen;
  char *lo, *hi;   /* the usable stack */
  int kind;
} sd_stack;
enum { SD_STACK_NONE = 0, SD_STACK_GUARDED, SD_STACK_SLAB };

/* Park states (sd_task.ps). A goroutine runs in RUNNING; it parks by
 * switching to its M's scheduler, which moves it to PARKED unless a waker
 * set NOTIFIED first. Exactly one waker unparks each park: the one that
 * claimed what the goroutine waits for (a select's done flag, a wait's
 * result, a group's waiter slot). */
enum { SD_PS_RUNNING = 0, SD_PS_NOTIFIED, SD_PS_PARKED, SD_PS_PARKED_COUNTED };

/* How a stop request or a handle close reaches a waiting goroutine
 * (sd_task.wait_mode): by claiming wait_res and unparking it, or through its
 * wake-up pipe (a wait polling in the OS). */
enum { SD_WM_NONE = 0, SD_WM_PARK, SD_WM_PIPE };
enum { SD_WR_PENDING = 0, SD_WR_READY, SD_WR_TIMEOUT, SD_WR_STOP, SD_WR_WOKEN };

struct sd_task {
  sd_frame *frame;   /* innermost protected frame */
  int raise;         /* pending raise kind */
  char *msg;         /* crash message (malloc'ed unless msg_static) */
  bool msg_static;

  sd_cleanup *cl;
  size_t ncl, capcl;
  char *arena;
  size_t arena_len, arena_cap;
  sd_lframe *lf_top; /* the generated code's cleanup frames (seed.h) */

  pthread_mutex_t mu; /* protects sel, waiting_group, wait_mode, wake_r/wake_w */
  _Atomic int ps;     /* SD_PS_* */
  sd_sel *sel;
  sd_group *waiting_group;
  atomic_int cancelled;
  /* Blocking waits: the result, claimed by the first waker; how wakers reach
   * the wait; the wake-up pipe of a wait that polls in the OS (created on
   * first use, -1 before); the wait's deadline. */
  _Atomic int wait_res;
  int wait_mode;
  int wake_r, wake_w;
  sd_timer wtimer;
  sd_task *hnext;      /* in a handle's queue of goroutines waiting for its operation lock */
  int blockdepth;      /* nesting of sd_block_begin */

  /* A thread the scheduler does not run (a unit test's main thread, say)
   * that uses the runtime: it parks on pmu/cv. */
  bool foreign;
  pthread_mutex_t pmu;
  pthread_cond_t cv;

  sd_group *group;          /* the group this goroutine belongs to (NULL for main) */
  sd_task *next, *prev;     /* the other running members of that group */
  sd_task_fn body;
  void *args;
  const sd_type *args_type;
  sd_err result;            /* what the body returned, handed to the group on g0 */
  _Alignas(16) char argbuf[64]; /* the arguments, when they fit */
  uint64_t rng;
  void (**atexit_fns)(void); /* sd_goroutine_atexit, run when the goroutine ends */
  size_t natexit, capatexit;
  void **gls;                /* goroutine-local storage (sd_goroutine_local) */
  size_t ngls;
  /* For the deadlock report: the goroutine's name (the spawn expression,
   * a test's description, or NULL) and what it is parked on. */
  const char *name;
  int blocked_kind; /* SD_BLOCK_* */
  int blocked_n;    /* number of cases of a select */

  /* Scheduling. */
  void *sp;           /* saved stack pointer (or ucontext) while switched out */
  sd_stack stk;
  sd_task *schedlink; /* in a run queue or a free list */
  void *tsan_fiber;
};

enum { SD_BLOCK_NONE = 0, SD_BLOCK_RECV, SD_BLOCK_SEND, SD_BLOCK_SELECT, SD_BLOCK_GROUP };

struct sd_group {
  pthread_mutex_t mu;
  sd_task *members; /* running goroutines */
  sd_task *dead;    /* finished goroutines not freed yet (linked by next) */
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

/* A processor: the right to run goroutines, with a run queue. The owner (the
 * M holding it) pushes and pops at the queue's tail and head; other Ms steal
 * half of it from the head (Go's runqput/runqget/runqsteal). runnext is the
 * goroutine to run next: the one a goroutine just readied or spawned, so
 * that a hand-off runs on the same CPU. */
enum { SD_P_IDLE = 0, SD_P_RUNNING, SD_P_SYSCALL };

struct sd_p {
  int id;
  _Atomic uint32_t head, tail;
  _Atomic(sd_task *) q[SD_RUNQ];
  _Atomic(sd_task *) runnext;
  _Atomic int status;            /* SD_P_* */
  _Atomic uint32_t syscalltick;  /* counts block sections */
  _Atomic uint32_t schedtick;    /* counts goroutines run (written by the owner) */
  uint32_t sm_tick;              /* sysmon's view of syscalltick, and when it changed */
  int64_t sm_when;
  sd_p *link;                    /* in the idle list */
  sd_task *gfree;                /* finished goroutines kept for reuse */
  int ngfree;
  uint32_t ntrim;
};

/* A worker thread. g0sp is its own stack (the scheduler context), saved
 * while it runs a goroutine. */
enum { SD_ACT_NONE = 0, SD_ACT_PARK, SD_ACT_PARK_COUNTED, SD_ACT_EXIT, SD_ACT_BLOCKEND };

struct sd_m {
  void *g0sp;
  sd_task *curg;
  sd_p *p, *nextp;
  bool spinning;
  int act;        /* what the goroutine that switched to g0 asks for */
  sd_task *actg;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool woken;
  sd_m *link;     /* in the idle list */
  void *altstack;
  uint64_t rng;
  void *tsan_fiber;
  const void *g0_bottom; /* the thread's stack, for AddressSanitizer */
  size_t g0_size;
};

static struct {
  pthread_mutex_t mu;     /* the global run queue, the idle Ps and Ms */
  sd_task *runq_head, *runq_tail;
  _Atomic int64_t runq_size;
  sd_p *pidle;
  _Atomic int npidle;
  sd_m *midle;
  _Atomic int nmspinning;
  int nprocs;
  sd_p *allp;
  pthread_mutex_t gmu;    /* finished goroutines shared by the Ps */
  sd_task *gfree;
  _Atomic int ngfree;
} sched = {.mu = PTHREAD_MUTEX_INITIALIZER, .gmu = PTHREAD_MUTEX_INITIALIZER};

static pthread_once_t sched_once = PTHREAD_ONCE_INIT;
static sd_task *sd_main_task; /* the task of sd_main: the root of the deadlock report */
/* Goroutines not parked (waits in the OS count), plus pending timers. Every
 * thread changes it: it has a cache line of its own. */
static _Alignas(64) atomic_long sd_running;
static char sd_running_pad_[64 - sizeof(atomic_long)] __attribute__((unused));
static atomic_ulong sd_seed_counter;
static bool sd_fault_installed;
static size_t sd_pagesize;

/* ------------------------------------------------- thread-local storage
 *
 * A goroutine may continue on another thread after any call that can park,
 * but C compilers treat the address of a _Thread_local variable as constant
 * within a function and may compute it once across such calls. So these are
 * only read and written inside noinline functions, which compute it afresh
 * on each call; their callers keep the result (a task or an M), never the
 * address. The barrier keeps the compiler from treating them as const
 * functions whose calls could be merged. */
static _Thread_local sd_task *sd_cur_; /* the goroutine running on this thread */
static _Thread_local sd_m *sd_curm_;   /* this thread's M (NULL on other threads) */

#define SD_TLS_FENCE() __asm__ __volatile__("")
static SD_NOINLINE sd_task *tls_task(void) {
  SD_TLS_FENCE();
  return sd_cur_;
}
static SD_NOINLINE void tls_set_task(sd_task *t) {
  SD_TLS_FENCE();
  sd_cur_ = t;
}
static SD_NOINLINE sd_m *tls_m(void) {
  SD_TLS_FENCE();
  return sd_curm_;
}
static SD_NOINLINE void tls_set_m(sd_m *m) {
  SD_TLS_FENCE();
  sd_curm_ = m;
}

/* ------------------------------------------------------ context switch
 *
 * sd_ctx_swap_(save, to) saves the callee-saved registers on the current
 * stack, stores the stack pointer in *save, switches to the stack pointer
 * `to` and restores what is saved there. A new context starts in
 * sd_ctx_start_, which calls the function in a callee-saved register. */

#if !defined(SD_CTX_UCONTEXT)
#if defined(__APPLE__)
#define SD_ASM_FN(x) ".globl _" x "\n.private_extern _" x "\n.p2align 4\n_" x ":\n"
#define SD_ASM_END(x) ""
#define SD_ASM_BEGIN ".text\n"
#define SD_ASM_DONE ""
#elif defined(__x86_64__)
#define SD_ASM_FN(x) ".globl " x "\n.hidden " x "\n.type " x ", @function\n.p2align 4\n" x ":\n"
#define SD_ASM_END(x) ".size " x ", .-" x "\n"
#define SD_ASM_BEGIN ".pushsection .text\n"
#define SD_ASM_DONE ".popsection\n"
#else
#define SD_ASM_FN(x) ".globl " x "\n.hidden " x "\n.type " x ", %function\n.p2align 4\n" x ":\n"
#define SD_ASM_END(x) ".size " x ", .-" x "\n"
#define SD_ASM_BEGIN ".pushsection .text\n"
#define SD_ASM_DONE ".popsection\n"
#endif

void sd_ctx_swap_(void **save, void *to);
void sd_ctx_start_(void);

#if defined(__x86_64__)
/* System V: rbx, rbp, r12-r15, and the SSE and x87 control words. */
__asm__(SD_ASM_BEGIN
        SD_ASM_FN("sd_ctx_swap_")
        "  pushq %rbp\n"
        "  pushq %rbx\n"
        "  pushq %r12\n"
        "  pushq %r13\n"
        "  pushq %r14\n"
        "  pushq %r15\n"
        "  subq $8, %rsp\n"
        "  stmxcsr (%rsp)\n"
        "  fnstcw 4(%rsp)\n"
        "  movq %rsp, (%rdi)\n"
        "  movq %rsi, %rsp\n"
        "  ldmxcsr (%rsp)\n"
        "  fldcw 4(%rsp)\n"
        "  addq $8, %rsp\n"
        "  popq %r15\n"
        "  popq %r14\n"
        "  popq %r13\n"
        "  popq %r12\n"
        "  popq %rbx\n"
        "  popq %rbp\n"
        "  ret\n"
        SD_ASM_END("sd_ctx_swap_")
        SD_ASM_FN("sd_ctx_start_")
        "  callq *%r13\n"
        "  ud2\n"
        SD_ASM_END("sd_ctx_start_")
        SD_ASM_DONE);

/* The frame sd_ctx_swap_ restores: control words, r15, r14, r13, r12, rbx,
 * rbp, return address. */
static void *ctx_make(char *top, void (*fn)(void)) {
  uintptr_t t = (uintptr_t)top & ~(uintptr_t)15;
  uint64_t *sp = (uint64_t *)(t - 128); /* 16-aligned, so the entry's rsp is too after the ret */
  memset(sp, 0, 128);
  sp[0] = 0x1F80u | ((uint64_t)0x037Fu << 32);
  sp[3] = (uint64_t)(uintptr_t)fn; /* r13 */
  sp[7] = (uint64_t)(uintptr_t)sd_ctx_start_;
  return sp;
}
#else
/* AAPCS64 (Linux and Apple): x19-x28, x29 (frame), x30 (link), d8-d15. x18
 * is not touched (reserved on Apple). The hint is BTI's landing pad (a no-op
 * elsewhere). */
__asm__(SD_ASM_BEGIN
        SD_ASM_FN("sd_ctx_swap_")
        "  hint #34\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n"
        "  stp x21, x22, [sp, #16]\n"
        "  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n"
        "  stp x27, x28, [sp, #64]\n"
        "  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n"
        "  stp d10, d11, [sp, #112]\n"
        "  stp d12, d13, [sp, #128]\n"
        "  stp d14, d15, [sp, #144]\n"
        "  mov x9, sp\n"
        "  str x9, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n"
        "  ldp x21, x22, [sp, #16]\n"
        "  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n"
        "  ldp x27, x28, [sp, #64]\n"
        "  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n"
        "  ldp d10, d11, [sp, #112]\n"
        "  ldp d12, d13, [sp, #128]\n"
        "  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #176\n"
        "  ret\n"
        SD_ASM_END("sd_ctx_swap_")
        SD_ASM_FN("sd_ctx_start_")
        "  hint #34\n"
        "  blr x20\n"
        "  brk #1\n"
        SD_ASM_END("sd_ctx_start_")
        SD_ASM_DONE);

/* The frame sd_ctx_swap_ restores (176 bytes): x19..x30, d8..d15. */
static void *ctx_make(char *top, void (*fn)(void)) {
  uintptr_t t = (uintptr_t)top & ~(uintptr_t)15;
  uint64_t *sp = (uint64_t *)(t - 176 - 16);
  memset(sp, 0, 176);
  sp[1] = (uint64_t)(uintptr_t)fn;            /* x20 */
  sp[11] = (uint64_t)(uintptr_t)sd_ctx_start_; /* x30 */
  return sp;
}
#endif

static inline void ctx_swap(void **save, void *to) { sd_ctx_swap_(save, to); }

#else /* SD_CTX_UCONTEXT: other architectures, through ucontext (slower: it
       * saves the signal mask with a system call). A context is a
       * ucontext_t, kept with the goroutine or the M. */
static void ctx_swap(void **save, void *to) {
  ucontext_t *cur = *save;
  if (!cur) {
    cur = malloc(sizeof *cur);
    if (!cur) sd_fatal("out of memory");
    *save = cur;
  }
  swapcontext(cur, (ucontext_t *)to);
}
#endif

/* ------------------------------------------------------------------ stacks
 *
 * A goroutine's stack is reserved (MAP_NORESERVE: memory is used only as it
 * grows) with SD_STACK_GUARD of inaccessible memory below it, so that a stack
 * overflow faults (the fault handler reports it as a crash of the
 * goroutine). Each guarded stack costs two kernel memory maps; Linux limits
 * them (vm.max_map_count, 65530 by default), so beyond a budget taken from
 * that limit, stacks come from shared mappings without guards (an overflow
 * there is not caught). Stacks are pooled with their goroutines. */

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifdef MAP_NORESERVE
#define SD_MAP_NORESERVE MAP_NORESERVE
#else
#define SD_MAP_NORESERVE 0
#endif
#ifdef MAP_STACK
#define SD_MAP_STACK MAP_STACK
#else
#define SD_MAP_STACK 0
#endif

static pthread_mutex_t stack_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t stack_budget;         /* guarded stacks allowed at once */
static atomic_size_t stacks_guarded;
static char **slab_free;            /* unguarded stacks, SD_GOROUTINE_STACK each */
static size_t nslab_free, capslab_free;

static size_t page_round(size_t n) { return (n + sd_pagesize - 1) & ~(sd_pagesize - 1); }

static void stack_budget_init(void) {
  size_t budget = SIZE_MAX;
#if defined(__linux__)
  FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
  if (f) {
    long long v = 0;
    if (fscanf(f, "%lld", &v) == 1 && v > 0) {
      /* Two maps per stack; leave room for everything else (a quarter of
       * the limit, at least 4096 maps). */
      long long rest = v / 4 > 4096 ? v / 4 : 4096;
      budget = v > rest + 2048 ? (size_t)((v - rest) / 2) : 1024;
    }
    fclose(f);
  }
#endif
  stack_budget = budget;
}

static bool stack_alloc(sd_stack *s, size_t size) {
  size = page_round(size);
  size_t guard = page_round(SD_STACK_GUARD);
  if (atomic_fetch_add(&stacks_guarded, 1) < stack_budget) {
    char *m = mmap(NULL, guard + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | SD_MAP_NORESERVE | SD_MAP_STACK,
                   -1, 0);
    if (m != MAP_FAILED) {
      if (mprotect(m, guard, PROT_NONE) == 0) {
        s->map = m;
        s->maplen = guard + size;
        s->lo = m + guard;
        s->hi = m + guard + size;
        s->kind = SD_STACK_GUARDED;
        return true;
      }
      munmap(m, guard + size);
    }
  }
  atomic_fetch_sub(&stacks_guarded, 1);
  if (size != page_round(SD_GOROUTINE_STACK)) return false;
  pthread_mutex_lock(&stack_mu);
  if (nslab_free == 0) {
    char *m = mmap(NULL, size * SD_SLAB_STACKS, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | SD_MAP_NORESERVE | SD_MAP_STACK, -1, 0);
    if (m == MAP_FAILED) {
      pthread_mutex_unlock(&stack_mu);
      return false;
    }
    if (capslab_free < SD_SLAB_STACKS) {
      size_t cap = capslab_free ? capslab_free * 2 : 64;
      slab_free = sd_realloc(slab_free, cap * sizeof *slab_free);
      capslab_free = cap;
    }
    for (int i = SD_SLAB_STACKS; i-- > 0;) slab_free[nslab_free++] = m + (size_t)i * size;
  }
  char *lo = slab_free[--nslab_free];
  pthread_mutex_unlock(&stack_mu);
  s->map = NULL;
  s->maplen = 0;
  s->lo = lo;
  s->hi = lo + size;
  s->kind = SD_STACK_SLAB;
  return true;
}

static void stack_free(sd_stack *s) {
  if (s->kind == SD_STACK_GUARDED) {
    munmap(s->map, s->maplen);
    atomic_fetch_sub(&stacks_guarded, 1);
  } else if (s->kind == SD_STACK_SLAB) {
    pthread_mutex_lock(&stack_mu);
    if (nslab_free == capslab_free) {
      capslab_free = capslab_free ? capslab_free * 2 : 64;
      slab_free = sd_realloc(slab_free, capslab_free * sizeof *slab_free);
    }
    slab_free[nslab_free++] = s->lo;
    pthread_mutex_unlock(&stack_mu);
  }
  s->kind = SD_STACK_NONE;
}

/* A stack going back to the pool: if it was used deeper than SD_STACK_KEEP
 * (a word there is no longer zero), its memory below that is given back. */
static void stack_trim(sd_stack *s) {
#if !defined(SD_ASAN)
  if ((size_t)(s->hi - s->lo) <= SD_STACK_KEEP + sd_pagesize) return;
  char *cut = s->hi - SD_STACK_KEEP;
  if (*(volatile uint64_t *)(void *)(cut - 64) == 0) return;
#if defined(__linux__)
  madvise(s->lo, (size_t)(cut - s->lo), MADV_DONTNEED); /* private anonymous memory reads as zeros again */
#else
  mmap(s->lo, (size_t)(cut - s->lo), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | SD_MAP_NORESERVE, -1, 0);
#endif
#else
  (void)s;
#endif
}

/* ---------------------------------------------------------------- tasks */

static uint64_t sd_rand(sd_task *t) {
  uint64_t x = t->rng;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  t->rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static uint64_t seed_new(const void *salt) {
  /* The select generator's seed: a per-process base (the clock, read once)
   * and a counter, so that no clock is read per goroutine. */
  static _Atomic uint64_t seed_base;
  uint64_t base = atomic_load_explicit(&seed_base, memory_order_relaxed);
  if (!base) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    base = ((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec) * 0x9E3779B97F4A7C15ULL | 1;
    atomic_store_explicit(&seed_base, base, memory_order_relaxed);
  }
  uint64_t x = base ^ ((uint64_t)(uintptr_t)salt << 7) ^ (atomic_fetch_add(&sd_seed_counter, 1) * 0xBF58476D1CE4E5B9ULL);
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x ? x : 0x853c49e6748fea9bULL;
}

static sd_task *task_alloc(void) {
  sd_task *t = calloc(1, sizeof *t);
  if (!t) sd_fatal("out of memory");
  pthread_mutex_init(&t->mu, NULL);
  pthread_mutex_init(&t->pmu, NULL);
  pthread_cond_init(&t->cv, NULL);
  atomic_init(&t->cancelled, 0);
  atomic_init(&t->ps, SD_PS_RUNNING);
  atomic_init(&t->wait_res, 0);
  t->wake_r = t->wake_w = -1;
  t->wtimer.idx = SD_TIMER_OFF;
  t->rng = seed_new(t);
  return t;
}

static void task_destroy(sd_task *t) {
  if (t->wake_r >= 0) {
    close(t->wake_r);
    close(t->wake_w);
  }
  pthread_mutex_destroy(&t->mu);
  pthread_mutex_destroy(&t->pmu);
  pthread_cond_destroy(&t->cv);
  free(t->cl);
  free(t->arena);
  free(t->atexit_fns);
  free(t->gls);
  free(t->msg_static ? NULL : t->msg);
  stack_free(&t->stk);
#if defined(SD_TSAN)
  if (t->tsan_fiber) __tsan_destroy_fiber(t->tsan_fiber);
#endif
  free(t);
}

/* Readies a goroutine that finished for reuse: its state is reset, its
 * stack and buffers kept. */
static void task_reset(sd_task *t) {
  if (t->wake_r >= 0) { /* descriptors are scarcer than memory */
    close(t->wake_r);
    close(t->wake_w);
    t->wake_r = t->wake_w = -1;
  }
  free(t->msg_static ? NULL : t->msg);
  t->msg = NULL;
  t->msg_static = false;
  t->frame = NULL;
  t->raise = SD_RAISE_NONE;
  t->ncl = 0;
  t->arena_len = 0;
  t->lf_top = NULL;
  t->sel = NULL;
  t->waiting_group = NULL;
  atomic_store_explicit(&t->cancelled, 0, memory_order_relaxed);
  atomic_store_explicit(&t->ps, SD_PS_RUNNING, memory_order_relaxed);
  atomic_store_explicit(&t->wait_res, 0, memory_order_relaxed);
  t->wait_mode = SD_WM_NONE;
  t->wtimer.idx = SD_TIMER_OFF;
  t->hnext = NULL;
  t->blockdepth = 0;
  t->group = NULL;
  t->next = t->prev = NULL;
  t->body = NULL;
  t->args = NULL;
  t->args_type = NULL;
  t->natexit = 0;
  t->name = NULL;
  t->blocked_kind = SD_BLOCK_NONE;
  t->blocked_n = 0;
  t->schedlink = NULL;
}

static void sched_init(void);
static void g_entry(void);

static sd_p *cur_p(void) {
  sd_m *m = tls_m();
  return m && m->curg ? m->p : NULL;
}

/* A goroutine with a stack, from the current P's free list, the shared
 * one, or new. */
static sd_task *g_get(size_t stack) {
  pthread_once(&sched_once, sched_init);
  sd_task *t = NULL;
  sd_p *p = stack == SD_GOROUTINE_STACK ? cur_p() : NULL;
  if (p) {
    if (!p->gfree && atomic_load_explicit(&sched.ngfree, memory_order_relaxed) > 0) {
      /* refill half of the local list from the shared one */
      pthread_mutex_lock(&sched.gmu);
      while (sched.gfree && p->ngfree < SD_GFREE_LOCAL / 2) {
        sd_task *g = sched.gfree;
        sched.gfree = g->schedlink;
        sched.ngfree--;
        g->schedlink = p->gfree;
        p->gfree = g;
        p->ngfree++;
      }
      pthread_mutex_unlock(&sched.gmu);
    }
    t = p->gfree;
    if (t) {
      p->gfree = t->schedlink;
      p->ngfree--;
    }
  } else if (stack == SD_GOROUTINE_STACK && atomic_load_explicit(&sched.ngfree, memory_order_relaxed) > 0) {
    pthread_mutex_lock(&sched.gmu);
    t = sched.gfree;
    if (t) {
      sched.gfree = t->schedlink;
      sched.ngfree--;
    }
    pthread_mutex_unlock(&sched.gmu);
  }
  if (t) {
    t->schedlink = NULL;
    return t;
  }
  t = task_alloc();
  if (!stack_alloc(&t->stk, stack)) sd_fatal("cannot start a goroutine (out of memory for its stack)");
#if defined(SD_TSAN)
  t->tsan_fiber = __tsan_create_fiber(0);
#endif
  return t;
}

/* A finished goroutine goes back to a free list (or is freed). */
static void g_put(sd_task *t) {
  task_reset(t);
  if ((size_t)(t->stk.hi - t->stk.lo) != page_round(SD_GOROUTINE_STACK)) {
    task_destroy(t);
    return;
  }
#if defined(SD_ASAN)
  /* Frames of the goroutine that never returned leave their redzones
   * poisoned; the stack's top part is what the next one uses. */
  {
    size_t n = (size_t)(t->stk.hi - t->stk.lo);
    if (n > ((size_t)256 << 10)) n = (size_t)256 << 10;
    __asan_unpoison_memory_region(t->stk.hi - n, n);
  }
#endif
  sd_p *p = cur_p();
  if (p) {
    /* A stack that went deep gives its memory back (stack_trim). Looking
     * costs a page of the stack the goroutine did not use, so a P checks
     * every fourth one it keeps, and every one it passes on. */
    if ((++p->ntrim & 3) == 0) stack_trim(&t->stk);
    t->schedlink = p->gfree;
    p->gfree = t;
    if (++p->ngfree <= SD_GFREE_LOCAL) return;
    /* move half to the shared list */
    sd_task *batch = NULL;
    int n = 0;
    while (p->ngfree > SD_GFREE_LOCAL / 2) {
      sd_task *g = p->gfree;
      p->gfree = g->schedlink;
      p->ngfree--;
      g->schedlink = batch;
      batch = g;
      stack_trim(&g->stk);
      n++;
    }
    pthread_mutex_lock(&sched.gmu);
    sd_task *extra = NULL;
    while (batch) {
      sd_task *g = batch;
      batch = g->schedlink;
      if (sched.ngfree < SD_GFREE_GLOBAL) {
        g->schedlink = sched.gfree;
        sched.gfree = g;
        sched.ngfree++;
      } else {
        g->schedlink = extra;
        extra = g;
      }
    }
    pthread_mutex_unlock(&sched.gmu);
    while (extra) {
      sd_task *g = extra;
      extra = g->schedlink;
      task_destroy(g);
    }
    (void)n;
    return;
  }
  stack_trim(&t->stk);
  pthread_mutex_lock(&sched.gmu);
  bool keep = sched.ngfree < SD_GFREE_GLOBAL;
  if (keep) {
    t->schedlink = sched.gfree;
    sched.gfree = t;
    sched.ngfree++;
  }
  pthread_mutex_unlock(&sched.gmu);
  if (!keep) task_destroy(t);
}

/* The current goroutine. A thread that did not start through sd_main or
 * sd_spawn (a unit test, say) gets a root task on first use, which parks
 * by blocking its thread. */
static sd_task *task_current(void) {
  sd_task *t = tls_task();
  if (SD_UNLIKELY(!t)) {
    t = task_alloc();
    t->foreign = true;
    atomic_fetch_add(&sd_running, 1);
    tls_set_task(t);
  }
  return t;
}

void sd_task_set_name_(const char *name) { task_current()->name = name; }

/* Called by every generated function with a cleanup frame: reads the TLS
 * itself (noinline, never parks) rather than through tls_task. */
SD_NOINLINE sd_lframe **sd_lf_top_(void) {
  SD_TLS_FENCE();
  sd_task *t = sd_cur_;
  if (SD_UNLIKELY(!t)) t = task_current();
  return &t->lf_top;
}

/* ------------------------------------------------- deadlocks */

/* The deadlock report: one line per blocked goroutine, the goroutines of a
 * group indented under the goroutine that waits for it (or runs its block),
 * starting from main. Called when every goroutine is parked, so nothing
 * changes the tasks while it reads them. */
#define SD_DEADLOCK_LINES 20

typedef struct {
  char buf[4096];
  size_t len;
  int count, shown;
} deadlock_report;

static void report_put(deadlock_report *r, const char *fmt, ...) SD_PRINTF(2, 3);
static void report_put(deadlock_report *r, const char *fmt, ...) {
  if (r->len >= sizeof r->buf) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(r->buf + r->len, sizeof r->buf - r->len, fmt, ap);
  va_end(ap);
  if (n > 0) r->len += (size_t)n;
  if (r->len > sizeof r->buf) r->len = sizeof r->buf;
}

static void report_task(deadlock_report *r, sd_task *t, int depth);

static void report_group(deadlock_report *r, sd_group *g, int depth) {
  for (sd_task *m = g->members; m; m = m->next) report_task(r, m, depth);
}

static void report_task(deadlock_report *r, sd_task *t, int depth) {
  r->count++;
  if (r->shown < SD_DEADLOCK_LINES) {
    r->shown++;
    const char *what = "blocked";
    switch (t->blocked_kind) {
    case SD_BLOCK_RECV: what = "receiving from a channel"; break;
    case SD_BLOCK_SEND: what = "sending on a channel"; break;
    case SD_BLOCK_SELECT: what = "in a select"; break;
    case SD_BLOCK_GROUP: what = "waiting for its group's goroutines"; break;
    default: break;
    }
    report_put(r, "%*s", 2 + 2 * (depth < 10 ? depth : 10), "");
    if (t->name) report_put(r, "%.80s", t->name);
    else if (t == sd_main_task) report_put(r, "main()");
    else report_put(r, "a goroutine");
    if (t->blocked_kind == SD_BLOCK_SELECT) report_put(r, ": %s (%d cases)\n", what, t->blocked_n);
    else report_put(r, ": %s\n", what);
  }
  for (size_t i = 0; i < t->ncl; i++)
    if (t->cl[i].group) report_group(r, t->cl[i].group, depth + 1);
  sd_group *w = t->waiting_group;
  if (w) {
    bool seen = false;
    for (size_t i = 0; i < t->ncl; i++) seen = seen || t->cl[i].group == w;
    if (!seen) report_group(r, w, depth + 1);
  }
}

static SD_NORETURN void sd_deadlock(void) {
  fflush(stdout);
  static const char m[] = "crash: deadlock: all goroutines are blocked\n";
  if (write(2, m, sizeof m - 1) < 0) _exit(2);
  if (sd_main_task) {
    deadlock_report *r = calloc(1, sizeof *r);
    deadlock_report *head = calloc(1, sizeof *head);
    if (r && head) {
      report_task(r, sd_main_task, 0);
      report_put(head, "%d goroutine%s blocked:\n", r->count, r->count == 1 ? " is" : "s are");
      if (r->count > r->shown) report_put(r, "  ... and %d more\n", r->count - r->shown);
      if (write(2, head->buf, head->len) < 0 || write(2, r->buf, r->len) < 0) _exit(2);
    }
  }
  _exit(2);
}

static void running_dec(void) {
  if (atomic_fetch_sub(&sd_running, 1) == 1) sd_deadlock();
}

static int ncpu(void) {
  long c = sysconf(_SC_NPROCESSORS_ONLN);
  return c > 0 ? (int)c : 1;
}

static inline void cpu_relax(void) {
#if defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
  __asm__ __volatile__("pause");
#endif
}

static int64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Channel and group locks are held for a few instructions at a time, but
 * taken by several CPUs at once (a producer and a consumer, goroutines of
 * a group starting and ending in bursts), and a lock that blocks at once
 * costs a system call to sleep and another to wake. So they spin for a
 * while first: glibc's adaptive mutexes do (watching the lock without
 * writing to it); elsewhere, a short trylock loop. */
#if defined(__GLIBC__)
#define SD_SPIN_LOCKS 0
#else
#define SD_SPIN_LOCKS 1
#endif

static void spin_mutex_init(pthread_mutex_t *m) {
#if defined(__GLIBC__)
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, 3); /* PTHREAD_MUTEX_ADAPTIVE_NP (a _GNU_SOURCE name) */
  pthread_mutex_init(m, &a);
  pthread_mutexattr_destroy(&a);
#else
  pthread_mutex_init(m, NULL);
#endif
}

static void lock_briefly(pthread_mutex_t *m) {
#if SD_SPIN_LOCKS
  for (int i = 0; i < 100; i++) {
    if (pthread_mutex_trylock(m) == 0) return;
    cpu_relax();
  }
#endif
  pthread_mutex_lock(m);
}

/* ------------------------------------------------------------ run queues */

static bool runq_empty(sd_p *p) {
  for (;;) {
    uint32_t h = atomic_load_explicit(&p->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&p->tail, memory_order_acquire);
    sd_task *next = atomic_load_explicit(&p->runnext, memory_order_acquire);
    if (t == atomic_load_explicit(&p->tail, memory_order_acquire)) return h == t && !next;
  }
}

/* The global queue (sched.mu held). */
static void globrunq_put_locked(sd_task *g) {
  g->schedlink = NULL;
  if (sched.runq_tail) sched.runq_tail->schedlink = g;
  else sched.runq_head = g;
  sched.runq_tail = g;
  atomic_fetch_add(&sched.runq_size, 1);
}

static void globrunq_putbatch_locked(sd_task *head, sd_task *tail, int n) {
  tail->schedlink = NULL;
  if (sched.runq_tail) sched.runq_tail->schedlink = head;
  else sched.runq_head = head;
  sched.runq_tail = tail;
  atomic_fetch_add(&sched.runq_size, n);
}

static sd_task *globrunq_pop_locked(void) {
  sd_task *g = sched.runq_head;
  if (!g) return NULL;
  sched.runq_head = g->schedlink;
  if (!sched.runq_head) sched.runq_tail = NULL;
  atomic_fetch_sub(&sched.runq_size, 1);
  g->schedlink = NULL;
  return g;
}

static bool runq_put_slow(sd_p *p, sd_task *g, uint32_t h, uint32_t t);

/* Owner only: puts g at the tail (or in runnext, moving the old runnext to
 * the tail). A full queue moves half of itself to the global queue. */
static void runq_put(sd_p *p, sd_task *g, bool next) {
  if (next) {
    sd_task *old = atomic_exchange_explicit(&p->runnext, g, memory_order_acq_rel);
    if (!old) return;
    g = old;
  }
  for (;;) {
    uint32_t h = atomic_load_explicit(&p->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&p->tail, memory_order_relaxed);
    if (t - h < SD_RUNQ) {
      atomic_store_explicit(&p->q[t % SD_RUNQ], g, memory_order_relaxed);
      atomic_store_explicit(&p->tail, t + 1, memory_order_release);
      return;
    }
    if (runq_put_slow(p, g, h, t)) return;
  }
}

static bool runq_put_slow(sd_p *p, sd_task *g, uint32_t h, uint32_t t) {
  enum { N = SD_RUNQ / 2 };
  sd_task *batch[N + 1];
  uint32_t n = (t - h) / 2;
  if (n != N) return false; /* the queue changed: retry */
  for (uint32_t i = 0; i < n; i++) batch[i] = atomic_load_explicit(&p->q[(h + i) % SD_RUNQ], memory_order_relaxed);
  if (!atomic_compare_exchange_strong_explicit(&p->head, &h, h + n, memory_order_acq_rel, memory_order_acquire))
    return false;
  batch[n] = g;
  for (uint32_t i = 0; i < n; i++) batch[i]->schedlink = batch[i + 1];
  pthread_mutex_lock(&sched.mu);
  globrunq_putbatch_locked(batch[0], batch[n], (int)n + 1);
  pthread_mutex_unlock(&sched.mu);
  return true;
}

/* Owner only: runnext first, then the head. */
static sd_task *runq_get(sd_p *p) {
  sd_task *next = atomic_load_explicit(&p->runnext, memory_order_relaxed);
  if (next && atomic_compare_exchange_strong_explicit(&p->runnext, &next, NULL, memory_order_acq_rel, memory_order_relaxed))
    return next;
  for (;;) {
    uint32_t h = atomic_load_explicit(&p->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&p->tail, memory_order_relaxed);
    if (t == h) return NULL;
    sd_task *g = atomic_load_explicit(&p->q[h % SD_RUNQ], memory_order_relaxed);
    if (atomic_compare_exchange_strong_explicit(&p->head, &h, h + 1, memory_order_acq_rel, memory_order_acquire)) return g;
  }
}

/* Takes half of p's queue into batch (a ring of SD_RUNQ starting at bhead).
 * With steal_next, takes p's runnext if the queue is empty, after giving its
 * owner a few microseconds to run it (a goroutine that readies another and
 * parks right away hands its CPU over, which is faster than a steal). */
static uint32_t runq_grab(sd_p *p, _Atomic(sd_task *) *batch, uint32_t bhead, bool steal_next) {
  for (;;) {
    uint32_t h = atomic_load_explicit(&p->head, memory_order_acquire);
    uint32_t t = atomic_load_explicit(&p->tail, memory_order_acquire);
    uint32_t n = t - h;
    n -= n / 2;
    if (n == 0) {
      if (steal_next) {
        sd_task *next = atomic_load_explicit(&p->runnext, memory_order_acquire);
        if (next) {
          if (atomic_load_explicit(&p->status, memory_order_relaxed) == SD_P_RUNNING) {
            /* As Go does: a 3 us sleep, which Linux's timer slack makes
             * about 50 us. Stealing it at once would split a goroutine
             * that hands values to another (and parks) from that one: each
             * hand-off would then wake a thread, and a buffered channel
             * would be fought over by two CPUs. */
            uint32_t tick = atomic_load_explicit(&p->schedtick, memory_order_relaxed);
            struct timespec ts = {0, 3000};
            nanosleep(&ts, NULL);
            /* The owner ran something meanwhile (maybe this goroutine, which
             * is back in runnext after a round): leave it. */
            if (atomic_load_explicit(&p->schedtick, memory_order_relaxed) != tick) return 0;
          }
          if (atomic_compare_exchange_strong_explicit(&p->runnext, &next, NULL, memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&batch[bhead % SD_RUNQ], next, memory_order_relaxed);
            return 1;
          }
        }
      }
      return 0;
    }
    if (n > SD_RUNQ / 2) continue; /* read an inconsistent head and tail */
    for (uint32_t i = 0; i < n; i++) {
      sd_task *g = atomic_load_explicit(&p->q[(h + i) % SD_RUNQ], memory_order_relaxed);
      atomic_store_explicit(&batch[(bhead + i) % SD_RUNQ], g, memory_order_relaxed);
    }
    if (atomic_compare_exchange_strong_explicit(&p->head, &h, h + n, memory_order_acq_rel, memory_order_acquire)) return n;
  }
}

/* Steals half of p2's queue into p's (owned by the caller); returns one of
 * the goroutines to run. */
static sd_task *runq_steal(sd_p *p, sd_p *p2, bool steal_next) {
  uint32_t t = atomic_load_explicit(&p->tail, memory_order_relaxed);
  uint32_t n = runq_grab(p2, p->q, t, steal_next);
  if (n == 0) return NULL;
  n--;
  sd_task *g = atomic_load_explicit(&p->q[(t + n) % SD_RUNQ], memory_order_relaxed);
  if (n == 0) return g;
  atomic_store_explicit(&p->tail, t + n, memory_order_release);
  return g;
}

/* Takes goroutines from the global queue into p's (sched.mu held); returns
 * one to run. */
static sd_task *globrunq_get_locked(sd_p *p, int max) {
  int64_t size = atomic_load_explicit(&sched.runq_size, memory_order_relaxed);
  if (size == 0) return NULL;
  int64_t n = size / sched.nprocs + 1;
  if (n > size) n = size;
  if (max > 0 && n > max) n = max;
  if (n > SD_RUNQ / 2) n = SD_RUNQ / 2;
  sd_task *g = globrunq_pop_locked();
  for (int64_t i = 1; i < n; i++) runq_put(p, globrunq_pop_locked(), false);
  return g;
}

/* ------------------------------------------------------- idle Ps and Ms */

static void pidle_put_locked(sd_p *p) {
  atomic_store_explicit(&p->status, SD_P_IDLE, memory_order_release);
  p->link = sched.pidle;
  sched.pidle = p;
  atomic_fetch_add(&sched.npidle, 1);
}

static sd_p *pidle_get_locked(void) {
  sd_p *p = sched.pidle;
  if (p) {
    sched.pidle = p->link;
    atomic_fetch_sub(&sched.npidle, 1);
  }
  return p;
}

static void acquirep(sd_m *m, sd_p *p) {
  m->p = p;
  atomic_store_explicit(&p->status, SD_P_RUNNING, memory_order_release);
}

static void *m_main(void *arg);

static void m_wake(sd_m *m) {
  pthread_mutex_lock(&m->mu);
  m->woken = true;
  pthread_cond_signal(&m->cv);
  pthread_mutex_unlock(&m->mu);
}

static sd_m *m_new(void) {
  sd_m *m = calloc(1, sizeof *m);
  if (!m) sd_fatal("out of memory");
  pthread_mutex_init(&m->mu, NULL);
  pthread_cond_init(&m->cv, NULL);
  m->rng = seed_new(m);
  return m;
}

/* Runs p on an idle M, or on a new one (spinning: the M looks for work to
 * steal). Without p, takes an idle P; if there is none, does nothing. */
static void startm(sd_p *p, bool spinning) {
  pthread_mutex_lock(&sched.mu);
  if (!p) {
    p = pidle_get_locked();
    if (!p) {
      pthread_mutex_unlock(&sched.mu);
      if (spinning) atomic_fetch_sub(&sched.nmspinning, 1);
      return;
    }
  }
  sd_m *m = sched.midle;
  if (m) sched.midle = m->link;
  pthread_mutex_unlock(&sched.mu);
  if (m) {
    m->spinning = spinning;
    m->nextp = p;
    m_wake(m);
    return;
  }
  m = m_new();
  m->spinning = spinning;
  m->nextp = p;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t th;
  int err = pthread_create(&th, &attr, m_main, m);
  pthread_attr_destroy(&attr);
  if (err) sd_fatal("cannot start a worker thread");
}

/* Starts an M to steal work if a P is idle and no M is spinning already. */
static void wakep(void) {
  if (atomic_load_explicit(&sched.npidle, memory_order_acquire) == 0) return;
  int z = 0;
  if (atomic_load_explicit(&sched.nmspinning, memory_order_acquire) != 0 ||
      !atomic_compare_exchange_strong(&sched.nmspinning, &z, 1))
    return;
  startm(NULL, true);
}

/* Puts the M to sleep until startm gives it a P. */
static void stopm(sd_m *m) {
  pthread_mutex_lock(&sched.mu);
  m->link = sched.midle;
  sched.midle = m;
  pthread_mutex_unlock(&sched.mu);
  pthread_mutex_lock(&m->mu);
  while (!m->woken) pthread_cond_wait(&m->cv, &m->mu);
  m->woken = false;
  pthread_mutex_unlock(&m->mu);
  acquirep(m, m->nextp);
  m->nextp = NULL;
}

/* Hands a P whose M is blocked in the OS to another M (sysmon). */
static void handoffp(sd_p *p) {
  if (!runq_empty(p) || atomic_load(&sched.runq_size) > 0) {
    startm(p, false);
    return;
  }
  int z = 0;
  if (atomic_load(&sched.nmspinning) + atomic_load(&sched.npidle) == 0 &&
      atomic_compare_exchange_strong(&sched.nmspinning, &z, 1)) {
    startm(p, true);
    return;
  }
  pthread_mutex_lock(&sched.mu);
  if (atomic_load(&sched.runq_size) > 0) {
    pthread_mutex_unlock(&sched.mu);
    startm(p, false);
    return;
  }
  pidle_put_locked(p);
  pthread_mutex_unlock(&sched.mu);
}

/* Makes g runnable: on the current P (to run next), or on the global queue
 * from a thread without one. */
static void ready(sd_task *g) {
  sd_m *m = tls_m();
  if (m && m->p && atomic_load_explicit(&m->p->status, memory_order_relaxed) == SD_P_RUNNING) {
    runq_put(m->p, g, true);
  } else {
    pthread_mutex_lock(&sched.mu);
    globrunq_put_locked(g);
    pthread_mutex_unlock(&sched.mu);
  }
  wakep();
}

/* ------------------------------------------------------- parking */

static void mcall(sd_task *t, int act);

/* Blocks until someone calls unpark. The caller makes itself findable by
 * its waker before (and must not hold a lock). counted: the goroutine is
 * blocked for the deadlock check (a channel, a group); a wait for the OS or
 * a timer is not. */
static void park(sd_task *t, bool counted) {
  if (t->foreign) {
    pthread_mutex_lock(&t->pmu);
    int exp = SD_PS_RUNNING;
    if (atomic_compare_exchange_strong(&t->ps, &exp, counted ? SD_PS_PARKED_COUNTED : SD_PS_PARKED)) {
      if (counted) running_dec();
      while (atomic_load(&t->ps) != SD_PS_NOTIFIED) pthread_cond_wait(&t->cv, &t->pmu);
    }
    atomic_store(&t->ps, SD_PS_RUNNING);
    pthread_mutex_unlock(&t->pmu);
    return;
  }
  if (atomic_load_explicit(&t->ps, memory_order_acquire) == SD_PS_NOTIFIED) {
    atomic_store_explicit(&t->ps, SD_PS_RUNNING, memory_order_relaxed);
    return;
  }
  mcall(t, counted ? SD_ACT_PARK_COUNTED : SD_ACT_PARK);
}

static void unpark(sd_task *t) {
  int old = atomic_exchange(&t->ps, SD_PS_NOTIFIED);
  if (old < SD_PS_PARKED) return; /* it has not parked yet: it will see NOTIFIED */
  if (old == SD_PS_PARKED_COUNTED) atomic_fetch_add(&sd_running, 1);
  if (t->foreign) {
    pthread_mutex_lock(&t->pmu);
    pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&t->pmu);
    return;
  }
  atomic_store_explicit(&t->ps, SD_PS_RUNNING, memory_order_relaxed);
  ready(t);
}

/* ------------------------------------------------- the scheduler (g0) */

#if defined(SD_CTX_UCONTEXT)
/* The goroutine's ucontext_t sits at the top of its stack. */
static void ctx_prepare(sd_task *t) {
  ucontext_t *uc = (ucontext_t *)(((uintptr_t)t->stk.hi - sizeof(ucontext_t)) & ~(uintptr_t)63);
  if (getcontext(uc) != 0) sd_fatal("getcontext failed");
  uc->uc_stack.ss_sp = t->stk.lo;
  uc->uc_stack.ss_size = (size_t)((char *)uc - t->stk.lo) & ~(size_t)63;
  uc->uc_link = NULL;
  makecontext(uc, g_entry, 0);
  t->sp = uc;
}
#else
static void ctx_prepare(sd_task *t) { t->sp = ctx_make(t->stk.hi, g_entry); }
#endif

/* g0 -> g: runs g until it switches back. */
static void switch_to_g(sd_m *m, sd_task *g) {
#if defined(SD_TSAN)
  __tsan_switch_to_fiber(g->tsan_fiber, 0);
#endif
#if defined(SD_ASAN)
  void *fake = NULL;
  __sanitizer_start_switch_fiber(&fake, g->stk.lo, (size_t)(g->stk.hi - g->stk.lo));
#endif
  ctx_swap(&m->g0sp, g->sp);
#if defined(SD_ASAN)
  __sanitizer_finish_switch_fiber(fake, NULL, NULL);
#endif
}

/* g -> g0, asking it for act. Returns when g runs again, maybe on another
 * M (so nothing read before the switch about the thread is valid after). */
static void mcall(sd_task *t, int act) {
  sd_m *m = tls_m();
  m->act = act;
  m->actg = t;
#if defined(SD_TSAN)
  __tsan_switch_to_fiber(m->tsan_fiber, 0);
#endif
#if defined(SD_ASAN)
  void *fake = NULL;
  __sanitizer_start_switch_fiber(act == SD_ACT_EXIT ? NULL : &fake, m->g0_bottom, m->g0_size);
#endif
  ctx_swap(&t->sp, m->g0sp);
#if defined(SD_ASAN)
  const void *b;
  size_t s;
  __sanitizer_finish_switch_fiber(fake, &b, &s);
  sd_m *m2 = tls_m();
  m2->g0_bottom = b;
  m2->g0_size = s;
#endif
}

/* Finds a goroutine to run, stealing from the other Ps; without one, gives
 * the P back and sleeps until there is work (m holds a P on return). */
static sd_task *find_runnable(sd_m *m) {
top:;
  sd_p *p = m->p;
  sd_task *g;
  if (atomic_load_explicit(&p->schedtick, memory_order_relaxed) % 61 == 0 && atomic_load_explicit(&sched.runq_size, memory_order_relaxed) > 0) {
    pthread_mutex_lock(&sched.mu);
    g = globrunq_get_locked(p, 1);
    pthread_mutex_unlock(&sched.mu);
    if (g) return g;
  }
  if ((g = runq_get(p))) return g;
  if (atomic_load_explicit(&sched.runq_size, memory_order_relaxed) > 0) {
    pthread_mutex_lock(&sched.mu);
    g = globrunq_get_locked(p, 0);
    pthread_mutex_unlock(&sched.mu);
    if (g) return g;
  }
  int procs = sched.nprocs;
  if (procs > 1 && (m->spinning || 2 * atomic_load(&sched.nmspinning) < procs - atomic_load(&sched.npidle))) {
    if (!m->spinning) {
      m->spinning = true;
      atomic_fetch_add(&sched.nmspinning, 1);
    }
    for (int i = 0; i < 4; i++) {
      m->rng = m->rng * 6364136223846793005ULL + 1442695040888963407ULL;
      int start = (int)((m->rng >> 33) % (uint64_t)procs);
      for (int k = 0; k < procs; k++) {
        sd_p *p2 = &sched.allp[(start + k) % procs];
        if (p2 == p) continue;
        if ((g = runq_steal(p, p2, i == 3))) return g;
      }
      if (atomic_load_explicit(&sched.runq_size, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&sched.mu);
        g = globrunq_get_locked(p, 0);
        pthread_mutex_unlock(&sched.mu);
        if (g) return g;
      }
    }
  }
  /* Nothing: give the P back. */
  pthread_mutex_lock(&sched.mu);
  if (atomic_load_explicit(&sched.runq_size, memory_order_relaxed) > 0) {
    g = globrunq_get_locked(p, 0);
    pthread_mutex_unlock(&sched.mu);
    return g;
  }
  m->p = NULL;
  pidle_put_locked(p);
  pthread_mutex_unlock(&sched.mu);
  if (m->spinning) {
    m->spinning = false;
    atomic_fetch_sub(&sched.nmspinning, 1);
    /* Work readied after this M looked but before it stopped spinning would
     * have woken no one (wakep saw a spinner): look again. */
    if (atomic_load(&sched.runq_size) > 0) {
      pthread_mutex_lock(&sched.mu);
      sd_p *np = atomic_load(&sched.runq_size) > 0 ? pidle_get_locked() : NULL;
      if (np) {
        acquirep(m, np);
        g = globrunq_get_locked(np, 0);
        pthread_mutex_unlock(&sched.mu);
        m->spinning = true;
        atomic_fetch_add(&sched.nmspinning, 1);
        return g;
      }
      pthread_mutex_unlock(&sched.mu);
    }
    for (int k = 0; k < procs; k++) {
      if (!runq_empty(&sched.allp[k])) {
        pthread_mutex_lock(&sched.mu);
        sd_p *np = pidle_get_locked();
        pthread_mutex_unlock(&sched.mu);
        if (np) {
          acquirep(m, np);
          m->spinning = true;
          atomic_fetch_add(&sched.nmspinning, 1);
          goto top;
        }
        break;
      }
    }
  }
  stopm(m);
  goto top;
}

static void goroutine_exit_g0(sd_task *t);

/* What a goroutine that switched to g0 asked for. Returns the goroutine to
 * run next (the same one, readied meanwhile), or NULL. */
static sd_task *handle_act(sd_m *m, sd_task *g) {
  int act = m->act;
  m->act = SD_ACT_NONE;
  m->actg = NULL;
  switch (act) {
  case SD_ACT_PARK:
  case SD_ACT_PARK_COUNTED: {
    bool counted = act == SD_ACT_PARK_COUNTED;
    int exp = SD_PS_RUNNING;
    if (atomic_compare_exchange_strong(&g->ps, &exp, counted ? SD_PS_PARKED_COUNTED : SD_PS_PARKED)) {
      if (counted) running_dec();
      return NULL;
    }
    atomic_store_explicit(&g->ps, SD_PS_RUNNING, memory_order_relaxed); /* woken meanwhile */
    return g;
  }
  case SD_ACT_EXIT:
    goroutine_exit_g0(g);
    return NULL;
  case SD_ACT_BLOCKEND: {
    /* g left a block section and its P is gone: run it on an idle P, or
     * queue it for the next one. */
    pthread_mutex_lock(&sched.mu);
    sd_p *p = pidle_get_locked();
    if (!p) globrunq_put_locked(g);
    pthread_mutex_unlock(&sched.mu);
    if (p) {
      acquirep(m, p);
      return g;
    }
    return NULL;
  }
  default: return NULL;
  }
}

/* The scheduler loop on an M's own stack. */
static SD_NORETURN void schedule(sd_m *m) {
  for (;;) {
    if (!m->p) stopm(m);
    sd_task *g = find_runnable(m);
    if (m->spinning) {
      /* Found work while spinning: let another M spin for what is left. */
      m->spinning = false;
      atomic_fetch_sub(&sched.nmspinning, 1);
      wakep();
    }
    while (g) {
      m->curg = g;
      tls_set_task(g);
      atomic_store_explicit(&m->p->schedtick, atomic_load_explicit(&m->p->schedtick, memory_order_relaxed) + 1,
                            memory_order_relaxed);
      switch_to_g(m, g);
      tls_set_task(NULL);
      m->curg = NULL;
      g = handle_act(m, g);
      if (g && !m->p) {
        pthread_mutex_lock(&sched.mu);
        globrunq_put_locked(g);
        pthread_mutex_unlock(&sched.mu);
        g = NULL;
      }
    }
  }
}

static void *altstack_on(void);

static void m_init_thread(sd_m *m) {
  tls_set_m(m);
  m->altstack = altstack_on();
#if defined(SD_TSAN)
  m->tsan_fiber = __tsan_get_current_fiber();
#endif
}

static void *m_main(void *arg) {
  sd_m *m = arg;
  m_init_thread(m);
  acquirep(m, m->nextp);
  m->nextp = NULL;
  schedule(m);
}

/* ------------------------------------------------- block sections, sysmon
 *
 * A goroutine about to block in the OS (a file, a FIFO's open, stdout into
 * a full pipe, waitpid) marks its P "in a system call". If the call takes
 * longer than one sysmon tick (20 us, up to 10 ms when nothing happens),
 * sysmon hands the P to another M, so the other goroutines keep running
 * however many are blocked. When the call returns, the goroutine takes its
 * P back if it is still there, or an idle one, or queues itself. */

static pthread_once_t sysmon_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t sysmon_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sysmon_cv = PTHREAD_COND_INITIALIZER;
static atomic_int sysmon_idle; /* sysmon sleeps until a block section begins */

static void *sysmon_main(void *arg) {
  (void)arg;
  int64_t delay = 20000;
  int quiet = 0;
  for (;;) {
    struct timespec ts = {0, (long)delay};
    nanosleep(&ts, NULL);
    int64_t now = mono_ns();
    bool insys = false, took = false;
    for (int i = 0; i < sched.nprocs; i++) {
      sd_p *p = &sched.allp[i];
      if (atomic_load_explicit(&p->status, memory_order_acquire) != SD_P_SYSCALL) continue;
      insys = true;
      uint32_t tick = atomic_load_explicit(&p->syscalltick, memory_order_relaxed);
      if (p->sm_tick != tick) { /* another block section since the last look */
        p->sm_tick = tick;
        p->sm_when = now;
        continue;
      }
      /* The same section since the last look. Leave it alone if the P has
       * nothing queued and other Ps are idle, for up to 10 ms. */
      if (runq_empty(p) && atomic_load(&sched.nmspinning) + atomic_load(&sched.npidle) > 0 && now - p->sm_when < 10000000)
        continue;
      int exp = SD_P_SYSCALL;
      if (atomic_compare_exchange_strong(&p->status, &exp, SD_P_IDLE)) {
        handoffp(p);
        took = true;
      }
    }
    if (took) {
      delay = 20000;
      quiet = 0;
    } else if (++quiet > 50 && delay < 10000000) {
      delay *= 2;
    }
    if (!insys && quiet > 200) {
      /* Nothing in a block section for a while: sleep until one begins. */
      pthread_mutex_lock(&sysmon_mu);
      atomic_store(&sysmon_idle, 1);
      bool any = false;
      for (int i = 0; i < sched.nprocs && !any; i++)
        any = atomic_load(&sched.allp[i].status) == SD_P_SYSCALL;
      while (!any && atomic_load(&sysmon_idle)) pthread_cond_wait(&sysmon_cv, &sysmon_mu);
      atomic_store(&sysmon_idle, 0);
      pthread_mutex_unlock(&sysmon_mu);
      delay = 20000;
      quiet = 0;
    }
  }
  return NULL;
}

static void sysmon_start(void) {
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, sysmon_main, NULL) != 0) sd_fatal("cannot start the scheduler's monitor thread");
  pthread_attr_destroy(&attr);
}

void sd_block_begin(void) {
  sd_m *m = tls_m();
  if (!m || !m->curg) return; /* a thread the scheduler does not run */
  sd_task *t = m->curg;
  if (t->blockdepth++ > 0) return;
  pthread_once(&sysmon_once, sysmon_start);
  sd_p *p = m->p;
  atomic_fetch_add_explicit(&p->syscalltick, 1, memory_order_relaxed);
  atomic_store_explicit(&p->status, SD_P_SYSCALL, memory_order_release);
  if (atomic_load_explicit(&sysmon_idle, memory_order_relaxed)) {
    pthread_mutex_lock(&sysmon_mu);
    atomic_store(&sysmon_idle, 0);
    pthread_cond_signal(&sysmon_cv);
    pthread_mutex_unlock(&sysmon_mu);
  }
}

static void block_end_now(sd_task *t) {
  sd_m *m = tls_m();
  sd_p *p = m->p;
  int exp = SD_P_SYSCALL;
  if (atomic_compare_exchange_strong(&p->status, &exp, SD_P_RUNNING)) return;
  /* sysmon gave the P to another M */
  m->p = NULL;
  pthread_mutex_lock(&sched.mu);
  sd_p *np = pidle_get_locked();
  pthread_mutex_unlock(&sched.mu);
  if (np) {
    acquirep(m, np);
    return;
  }
  mcall(t, SD_ACT_BLOCKEND);
}

void sd_block_end(void) {
  sd_m *m = tls_m();
  if (!m || !m->curg) return;
  sd_task *t = m->curg;
  if (t->blockdepth == 0 || --t->blockdepth > 0) return;
  int saved = errno;
  block_end_now(t);
  errno = saved;
}

/* ------------------------------------------------- sanity of the scheduler */

static int procs_from_env(void) {
  const char *s = getenv("SEED_PROCS");
  if (s && *s) {
    long v = strtol(s, NULL, 10);
    if (v >= 1) return v > SD_MAX_PROCS ? SD_MAX_PROCS : (int)v;
  }
  int n = ncpu();
  return n > SD_MAX_PROCS ? SD_MAX_PROCS : n;
}

static void sched_init(void) {
  long ps = sysconf(_SC_PAGESIZE);
  sd_pagesize = ps > 0 ? (size_t)ps : 4096;
  stack_budget_init();
  sched.nprocs = procs_from_env();
  sched.allp = calloc((size_t)sched.nprocs, sizeof *sched.allp);
  if (!sched.allp) sd_fatal("out of memory");
  pthread_mutex_lock(&sched.mu);
  for (int i = sched.nprocs; i-- > 0;) {
    sd_p *p = &sched.allp[i];
    p->id = i;
    atomic_init(&p->head, 0);
    atomic_init(&p->tail, 0);
    atomic_init(&p->runnext, NULL);
    atomic_init(&p->syscalltick, 0);
    for (int k = 0; k < SD_RUNQ; k++) atomic_init(&p->q[k], NULL);
    pidle_put_locked(p);
  }
  pthread_mutex_unlock(&sched.mu);
}

/* --------------------------------------------------- raising and frames */

void sd_free_queue_reset_(void);

/* Releases what the cleanup frames above mark hold (newest first), while
 * their C frames are still on the stack: called just before a siglongjmp
 * unwinds them. Each frame is unlinked before its release runs. */
static void lframes_release(sd_task *t, sd_lframe *mark) {
  sd_lframe *f;
  while ((f = t->lf_top) != NULL && f != mark) {
    t->lf_top = f->prev;
    f->release(f);
  }
}

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
  lframes_release(t, t->frame->lmark);
  siglongjmp(t->frame->jb, 1);
}

void sd_crash(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char *m = sd_vfmt(fmt, ap);
  va_end(ap);
  raise_in(task_current(), SD_RAISE_CRASH, m);
}

void sd_crash_owned(char *msg) { raise_in(task_current(), SD_RAISE_CRASH, msg); }

void sd_crash_index(sd_int i, int64_t len) {
  char *s = sd_int_to_cstr(i);
  char *m = sd_fmt("index out of range: %s with length %lld", s, (long long)len);
  free(s);
  sd_crash_owned(m);
}

void sd_crash_null(void) { sd_crash("null value used"); }

void sd_crash_int(const char *fmt, sd_int v) {
  char *s = sd_int_to_cstr(v);
  char *m = sd_fmt(fmt, s);
  free(s);
  sd_crash_owned(m);
}

static SD_NORETURN void stop_task(sd_task *t) { raise_in(t, SD_RAISE_STOP, NULL); }

static void check_cancel(sd_task *t) {
  if (SD_UNLIKELY(atomic_load_explicit(&t->cancelled, memory_order_relaxed))) stop_task(t);
}

static void unwind_to(sd_task *t, size_t mark);

/* Runs fn(arg) in a protected frame. Returns SD_RAISE_NONE when fn returned;
 * otherwise the cleanups fn left behind have been run and the pending raise
 * kind is returned (it stays pending in t->raise for the caller). The
 * signal mask is not saved: the fault handler runs with SA_NODEFER and an
 * empty mask, so a jump out of it leaves the mask as it was. */
static int protect(sd_task *t, void (*fn)(void *), void *arg) {
  sd_frame f;
  f.prev = t->frame;
  f.mark = t->ncl;
  f.lmark = t->lf_top;
  t->frame = &f;
  if (sigsetjmp(f.jb, 0) == 0) {
    fn(arg);
    t->frame = f.prev;
    return SD_RAISE_NONE;
  }
  /* A crash may have cut short a free in progress on this thread (its queue
   * is the thread's), or a block section. */
  sd_free_queue_reset_();
  if (t->blockdepth > 0) {
    t->blockdepth = 1;
    sd_block_end();
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

/* Goroutine-local storage: key is a static int, 0 before its first use
 * (then an index, given out once per program). The block is zeroed on the
 * goroutine's first use and freed when it ends, after its
 * sd_goroutine_atexit functions; its address never changes meanwhile. */
static atomic_int sd_gls_keys;

void *sd_goroutine_local(int *key, size_t size) {
  sd_task *t = task_current();
  int k = __atomic_load_n(key, __ATOMIC_ACQUIRE);
  if (SD_UNLIKELY(!k)) {
    int nk = atomic_fetch_add(&sd_gls_keys, 1) + 1, z = 0;
    k = __atomic_compare_exchange_n(key, &z, nk, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? nk : z;
  }
  size_t i = (size_t)k - 1;
  if (SD_UNLIKELY(i >= t->ngls)) {
    size_t n = t->ngls ? t->ngls : 8;
    while (n <= i) n *= 2;
    t->gls = sd_realloc(t->gls, n * sizeof *t->gls);
    memset(t->gls + t->ngls, 0, (n - t->ngls) * sizeof *t->gls);
    t->ngls = n;
  }
  void *p = t->gls[i];
  if (SD_UNLIKELY(!p)) {
    p = calloc(1, size ? size : 1);
    if (!p) sd_fatal("out of memory");
    t->gls[i] = p;
  }
  return p;
}

static void gls_free(sd_task *t) {
  for (size_t i = 0; i < t->ngls; i++) {
    free(t->gls[i]);
    t->gls[i] = NULL;
  }
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

static void wait_wake_locked(sd_task *m, int res);

static void task_cancel(sd_task *m) {
  atomic_store(&m->cancelled, 1);
  pthread_mutex_lock(&m->mu);
  wait_wake_locked(m, SD_WR_STOP);
  if (m->sel) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&m->sel->done, &expected, SEL_CANCELLED)) unpark(m);
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
  spin_mutex_init(&g->mu);
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

/* An M's alternate signal stack, where the fault handler runs (so that it
 * can report a stack overflow); NULL without the handler. */
static void *altstack_on(void) {
  if (!sd_fault_installed) return NULL;
  size_t sz = 64 * 1024;
  void *p = sd_alloc(sz);
  stack_t ss;
  memset(&ss, 0, sizeof ss);
  ss.ss_sp = p;
  ss.ss_size = sz;
  sigaltstack(&ss, NULL);
  return p;
}

/* A goroutine from start to end, on its own stack: its body, then the
 * release of its arguments, its sd_goroutine_atexit functions and its
 * goroutine-local storage; then (on g0, off its stack, which the group may
 * reuse at once) the report to its group. */
static SD_NORETURN void goroutine_main(sd_task *t) {
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
    if (t->args != (void *)t->argbuf) free(t->args);
    t->args = NULL;
  }
  while (t->natexit > 0) t->atexit_fns[--t->natexit]();
  gls_free(t);
  t->result = result;
  mcall(t, SD_ACT_EXIT);
  __builtin_unreachable();
}

/* On g0, after goroutine t returned (its result in t->result): moves it to
 * its group's dead list (its last use: the group may free it from then on). */
static void goroutine_exit_g0(sd_task *t) {
  sd_err result = t->result;
  t->result = SD_ERR_NULL;
  sd_group *g = t->group;
  lock_briefly(&g->mu);
  if (!sd_err_is_null(result)) {
    if (sd_err_is_null(g->err)) {
      g->err = result;
      result = SD_ERR_NULL;
    }
    group_cancel_locked(g);
  }
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
  running_dec();
}

/* Frees a list of finished goroutines (linked by next): they no longer run
 * (goroutine_exit_g0's last use is under the group's lock). */
static void reap(sd_task *m) {
  for (sd_task *next; m; m = next) {
    next = m->next;
    g_put(m);
  }
}

void sd_spawn(sd_group *g, sd_task_fn body, const sd_type *args_type, const void *args) {
  sd_task *parent = task_current();
  sd_task *c = g_get(SD_GOROUTINE_STACK);
  /* The select generator's seed, from the parent's (no shared counter). */
  uint64_t x = sd_rand(parent) ^ (uint64_t)(uintptr_t)c;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  c->rng = (x ^ (x >> 31)) | 1;
  c->body = body;
  c->group = g;
  c->args_type = args_type;
  /* Generated code names the descriptor of a spawn's arguments after the
   * spawn ("spawn worker(i, jobs)"): the goroutine's name in reports. */
  if (args_type && args_type->name && strncmp(args_type->name, "spawn ", 6) == 0) c->name = args_type->name + 6;
  if (args_type) {
    c->args = args_type->size <= sizeof c->argbuf ? (void *)c->argbuf : sd_alloc(args_type->size);
    sd_clone_deep(args_type, c->args, args);
  }
  lock_briefly(&g->mu);
  /* Finished goroutines of the group are freed here (outside the lock). */
  sd_task *dead = g->dead;
  g->dead = NULL;
  bool start = !g->failed && !atomic_load(&parent->cancelled);
  if (start) {
    c->next = g->members;
    if (c->next) c->next->prev = c;
    g->members = c;
    g->running++;
  }
  pthread_mutex_unlock(&g->mu);
  reap(dead);
  if (!start) {
    /* The group is already failing: the goroutine is not started. */
    if (args_type) {
      sd_release(args_type, c->args);
      if (c->args != (void *)c->argbuf) free(c->args);
      c->args = NULL;
    }
    c->next = NULL;
    g_put(c);
    return;
  }
  atomic_fetch_add(&sd_running, 1);
  ctx_prepare(c);
  ready(c);
}

/* Waits until every goroutine of g has finished and frees them. If the
 * waiting goroutine is cancelled, so is the group. */
static void group_join(sd_task *t, sd_group *g) {
  pthread_mutex_lock(&t->mu);
  t->waiting_group = g;
  t->blocked_kind = SD_BLOCK_GROUP;
  pthread_mutex_unlock(&t->mu);
  if (atomic_load(&t->cancelled)) group_cancel(g);
  pthread_mutex_lock(&g->mu);
  bool wait = g->running > 0;
  if (wait) g->waiter = t;
  pthread_mutex_unlock(&g->mu);
  if (wait) park(t, true);
  pthread_mutex_lock(&t->mu);
  t->waiting_group = NULL;
  t->blocked_kind = SD_BLOCK_NONE;
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
    char *m = sd_fmt("invalid channel buffer size: %s", s);
    free(s);
    sd_crash_owned(m);
  }
  sd_chan *c = sd_obj_alloc(sizeof *c);
  memset(c, 0, sizeof *c);
  atomic_init(&c->rc, 1);
  spin_mutex_init(&c->mu);
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

/* (spin_mutex_init and lock_briefly: a producer and a consumer on two
 * CPUs take a channel's lock all the time.) */
static void chan_lock(sd_chan *c) { lock_briefly(&c->mu); }

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
  chan_lock(c);
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
  for (int i = 0; i < n; i++) chan_lock(cs[i]);
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
    t->blocked_kind = n > 1 ? SD_BLOCK_SELECT : cases[0].dir == SD_RECV ? SD_BLOCK_RECV : SD_BLOCK_SEND;
    t->blocked_n = n;
    pthread_mutex_unlock(&t->mu);
    unlock_all(chans, nc);

    bool self_cancelled = false;
    if (atomic_load(&t->cancelled)) {
      int expected = 0;
      self_cancelled = atomic_compare_exchange_strong(&sel.done, &expected, SEL_CANCELLED);
    }
    if (!self_cancelled) park(t, true);
    pthread_mutex_lock(&t->mu);
    t->sel = NULL;
    t->blocked_kind = SD_BLOCK_NONE;
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

/* A send or receive is a select of one case; when it can proceed at once
 * (the common case of a buffered channel), it does without the select's
 * bookkeeping. */
void sd_chan_send(sd_chan *c, const void *val) {
  check_cancel(task_current());
  chan_lock(c);
  int r = try_send(c, val);
  pthread_mutex_unlock(&c->mu);
  if (r == 1) return;
  if (r == 0) sd_crash("send on closed channel");
  sd_select_case sc = {c, SD_SEND, (void *)val, false};
  sd_select(&sc, 1);
}

bool sd_chan_receive(sd_chan *c, void *out) {
  check_cancel(task_current());
  chan_lock(c);
  int r = try_recv(c, out);
  pthread_mutex_unlock(&c->mu);
  if (r >= 0) return r == 1;
  sd_select_case sc = {c, SD_RECV, out, false};
  sd_select(&sc, 1);
  return sc.ok;
}

/* ------------------------------------------------- timers and the poller
 *
 * One thread, the poller, waits in epoll (Linux) or kqueue (macOS, BSD) for
 * the descriptors goroutines wait on and for the earliest timer, and wakes
 * the goroutines. Timers are a heap: time.after's channels, and the
 * deadlines of waits. */

sd_int sd_time_now(void) { return sd_int_from_i64(mono_ns()); }

static pthread_mutex_t timer_mu = PTHREAD_MUTEX_INITIALIZER;
static sd_timer **timers;
static size_t ntimers, captimers;
static uint64_t timer_seq;

static bool timer_before(const sd_timer *a, const sd_timer *b) {
  return a->deadline < b->deadline || (a->deadline == b->deadline && a->seq < b->seq);
}

static void timer_set(size_t i, sd_timer *x) {
  timers[i] = x;
  x->idx = i;
}

static void timer_up(size_t i) {
  sd_timer *x = timers[i];
  while (i > 0 && timer_before(x, timers[(i - 1) / 2])) {
    timer_set(i, timers[(i - 1) / 2]);
    i = (i - 1) / 2;
  }
  timer_set(i, x);
}

static void timer_down(size_t i) {
  sd_timer *x = timers[i];
  for (;;) {
    size_t l = 2 * i + 1, r = l + 1, m = i;
    const sd_timer *best = x;
    if (l < ntimers && timer_before(timers[l], best)) {
      m = l;
      best = timers[l];
    }
    if (r < ntimers && timer_before(timers[r], best)) m = r;
    if (m == i) break;
    timer_set(i, timers[m]);
    i = m;
  }
  timer_set(i, x);
}

static void timer_push_locked(sd_timer *x) {
  if (ntimers == captimers) {
    captimers = captimers ? captimers * 2 : 16;
    timers = sd_realloc(timers, captimers * sizeof *timers);
  }
  x->seq = timer_seq++;
  timers[ntimers] = x;
  x->idx = ntimers++;
  timer_up(x->idx);
}

static void timer_remove_locked(sd_timer *x) {
  size_t i = x->idx;
  if (i == SD_TIMER_OFF) return;
  x->idx = SD_TIMER_OFF;
  sd_timer *last = timers[--ntimers];
  if (i == ntimers) return;
  timer_set(i, last);
  timer_down(i);
  timer_up(last->idx);
}

static pthread_once_t poller_once = PTHREAD_ONCE_INIT;
static bool poller_ok;      /* the poller runs (descriptors can be waited on through it) */
static int poll_fd = -1;    /* the epoll or kqueue descriptor */
static int64_t poll_armed;  /* deadline the poller sleeps until (timer_mu); 0: none */
#if defined(SD_EPOLL)
static int timer_fd = -1;
#define SD_POLL_TIMER_TOKEN UINT64_MAX
#endif

/* Makes the poller notice a new earliest deadline (timer_mu held). */
static void poller_arm_locked(int64_t deadline) {
  if (poll_armed && poll_armed <= deadline) return;
  poll_armed = deadline;
#if defined(SD_EPOLL)
  struct itimerspec its;
  memset(&its, 0, sizeof its);
  its.it_value.tv_sec = (time_t)(deadline / 1000000000);
  its.it_value.tv_nsec = (long)(deadline % 1000000000);
  if (its.it_value.tv_sec == 0 && its.it_value.tv_nsec == 0) its.it_value.tv_nsec = 1;
  timerfd_settime(timer_fd, TFD_TIMER_ABSTIME, &its, NULL);
#elif defined(SD_KQUEUE)
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  kevent(poll_fd, &ev, 1, NULL, 0, NULL);
#endif
}

/* Fires the due timers; returns the next deadline (0: none). */
static int64_t timers_run(void) {
  sd_timer *fire = NULL;
  pthread_mutex_lock(&timer_mu);
  int64_t now = mono_ns();
  while (ntimers > 0 && timers[0]->deadline <= now) {
    sd_timer *x = timers[0];
    timer_remove_locked(x);
    if (x->task) {
      /* a wait's deadline: fired under the lock, which the waiter takes to
       * remove it, so it never fires after the wait is over */
      int z = SD_WR_PENDING;
      if (atomic_compare_exchange_strong(&x->task->wait_res, &z, SD_WR_TIMEOUT)) unpark(x->task);
    } else {
      x->task = (sd_task *)fire; /* a list of channel timers, fired below */
      fire = x;
    }
  }
  int64_t next = ntimers ? timers[0]->deadline : 0;
#if defined(SD_EPOLL)
  poll_armed = 0;
  if (next) poller_arm_locked(next);
#else
  poll_armed = next; /* the poller sleeps until then (kevent's timeout) */
#endif
  pthread_mutex_unlock(&timer_mu);
  while (fire) {
    sd_timer *x = fire;
    fire = (sd_timer *)x->task;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    sd_int v = sd_int_from_i64((int64_t)wall.tv_sec * 1000000000 + wall.tv_nsec);
    pthread_mutex_lock(&x->c->mu);
    try_send(x->c, &v); /* buffer of one, sent once: never blocks */
    close_locked(x->c); /* later receives return null instead of waiting forever */
    pthread_mutex_unlock(&x->c->mu);
    sd_chan_release(x->c);
    free(x);
    running_dec();
  }
  return next;
}

/* The descriptors being waited on: a table of slots, each naming the
 * waiting goroutine, with a generation so that an event for a wait that
 * ended is ignored (poll_mu). */
typedef struct {
  sd_task *task;
  uint32_t gen;
  uint32_t next_free;
} poll_slot;

static pthread_mutex_t poll_mu = PTHREAD_MUTEX_INITIALIZER;
static poll_slot *pslots;
static uint32_t npslots, cappslots, pslot_free = UINT32_MAX;

static void poll_fire(uint64_t token) {
  uint32_t idx = (uint32_t)(token >> 32), gen = (uint32_t)token;
  pthread_mutex_lock(&poll_mu);
  if (idx < npslots && pslots[idx].gen == gen && pslots[idx].task) {
    sd_task *t = pslots[idx].task;
    int z = SD_WR_PENDING;
    if (atomic_compare_exchange_strong(&t->wait_res, &z, SD_WR_READY)) unpark(t);
  }
  pthread_mutex_unlock(&poll_mu);
}

static void *poller_main(void *arg) {
  (void)arg;
#if defined(SD_EPOLL)
  struct epoll_event evs[64];
  for (;;) {
    timers_run();
    int n = epoll_wait(poll_fd, evs, 64, -1);
    for (int i = 0; i < n; i++) {
      if (evs[i].data.u64 == SD_POLL_TIMER_TOKEN) {
        uint64_t x;
        ssize_t r = read(timer_fd, &x, sizeof x);
        (void)r;
        continue;
      }
      poll_fire(evs[i].data.u64);
    }
  }
#elif defined(SD_KQUEUE)
  struct kevent evs[64];
  for (;;) {
    int64_t next = timers_run();
    struct timespec ts, *tp = NULL;
    if (next) {
      int64_t d = next - mono_ns();
      if (d < 0) d = 0;
      ts.tv_sec = (time_t)(d / 1000000000);
      ts.tv_nsec = (long)(d % 1000000000);
      tp = &ts;
    }
    int n = kevent(poll_fd, NULL, 0, evs, 64, tp);
    for (int i = 0; i < n; i++) {
      if (evs[i].filter == EVFILT_USER) continue;
      poll_fire((uint64_t)(uintptr_t)evs[i].udata);
    }
  }
#else
  /* No poller: timers only, on a condition variable-free sleep loop. */
  for (;;) {
    int64_t next = timers_run();
    int64_t d = next ? next - mono_ns() : 1000000;
    if (d > 1000000) d = 1000000;
    if (d > 0) {
      struct timespec ts = {(time_t)(d / 1000000000), (long)(d % 1000000000)};
      nanosleep(&ts, NULL);
    }
  }
#endif
  return NULL;
}

static void poller_start(void) {
#if defined(SD_EPOLL)
  poll_fd = epoll_create1(EPOLL_CLOEXEC);
  timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (poll_fd < 0 || timer_fd < 0) sd_fatal("cannot create the poller (epoll)");
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = EPOLLIN;
  ev.data.u64 = SD_POLL_TIMER_TOKEN;
  if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, timer_fd, &ev) != 0) sd_fatal("cannot create the poller (timerfd)");
  poller_ok = true;
#elif defined(SD_KQUEUE)
  poll_fd = kqueue();
  if (poll_fd < 0) sd_fatal("cannot create the poller (kqueue)");
  fcntl(poll_fd, F_SETFD, FD_CLOEXEC);
  struct kevent ev;
  EV_SET(&ev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if (kevent(poll_fd, &ev, 1, NULL, 0, NULL) != 0) sd_fatal("cannot create the poller (kqueue user event)");
  poller_ok = true;
#endif
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, poller_main, NULL) != 0) sd_fatal("cannot start the timer thread");
  pthread_attr_destroy(&attr);
}

sd_chan *sd_time_after(sd_int ns) {
  int64_t d;
  if (!sd_int_to_i64(ns, &d) || d > INT64_MAX / 4) d = INT64_MAX / 4;
  if (d < 0) d = 0;
  sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(1));
  sd_timer *x = calloc(1, sizeof *x);
  if (!x) sd_fatal("out of memory");
  x->c = sd_chan_retain(c);
  x->deadline = mono_ns() + d;
  pthread_once(&poller_once, poller_start);
  atomic_fetch_add(&sd_running, 1); /* a pending timer can still wake someone */
  pthread_mutex_lock(&timer_mu);
  timer_push_locked(x);
  if (x->idx == 0) poller_arm_locked(x->deadline);
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

/* A native that waits for the OS (a socket, stdin, a child process) waits
 * through wait_core: its goroutine parks until the poller sees the
 * descriptor ready, its deadline passes, it is stopped (task_cancel) or,
 * for a handle's wait, the handle is closed. A descriptor the poller cannot
 * watch is polled in the OS, in a block section, together with the
 * goroutine's wake-up pipe, which a stop or a close writes to. A goroutine
 * waiting for the OS counts as running for the deadlock check: something
 * outside the program can still wake it. */

#define SD_WAIT_WOKEN (-3)       /* internal: woken without a stop request */
#define SD_WAIT_UNWATCHED (-4)   /* internal: the poller cannot watch this descriptor */
#define SD_WAIT_SLICE_MS 50      /* without a wake-up pipe, check for a stop this often */

static void fd_set_flags(int fd, bool nonblock) {
  fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  if (nonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/* With m->mu held: wakes m's wait, if any, with res (a stop or a close). */
static void wait_wake_locked(sd_task *m, int res) {
  if (m->wait_mode == SD_WM_PARK) {
    int z = SD_WR_PENDING;
    if (atomic_compare_exchange_strong(&m->wait_res, &z, res)) unpark(m);
  } else if (m->wait_mode == SD_WM_PIPE && m->wake_w >= 0) {
    char c = 1;
    ssize_t r = write(m->wake_w, &c, 1); /* a full pipe already has a wake-up pending */
    (void)r;
  }
}

static void wait_mode_set(sd_task *t, int mode) {
  pthread_mutex_lock(&t->mu);
  t->wait_mode = mode;
  pthread_mutex_unlock(&t->mu);
}

static int wait_result(int res) {
  switch (res) {
  case SD_WR_READY: return SD_WAIT_READY;
  case SD_WR_TIMEOUT: return SD_WAIT_TIMEOUT;
  case SD_WR_STOP: return SD_WAIT_STOP;
  default: return SD_WAIT_WOKEN;
  }
}

/* Claims the wait's result for the goroutine itself (before it parks).
 * False: a waker claimed it first and will unpark it. */
static bool wait_claim(sd_task *t, int res) {
  int z = SD_WR_PENDING;
  return atomic_compare_exchange_strong(&t->wait_res, &z, res);
}

/* Parks t until a waker sets its wait result (set up with wait_res pending
 * and wait_mode SD_WM_PARK), or its deadline (0: none). */
static int wait_park(sd_task *t, int64_t deadline) {
  if (atomic_load(&t->cancelled)) {
    if (wait_claim(t, SD_WR_STOP)) return SD_WR_STOP;
  } else if (deadline) {
    pthread_once(&poller_once, poller_start);
    t->wtimer.deadline = deadline;
    t->wtimer.task = t;
    t->wtimer.c = NULL;
    pthread_mutex_lock(&timer_mu);
    timer_push_locked(&t->wtimer);
    if (t->wtimer.idx == 0) poller_arm_locked(deadline);
    pthread_mutex_unlock(&timer_mu);
  }
  park(t, false);
  if (deadline) {
    pthread_mutex_lock(&timer_mu);
    timer_remove_locked(&t->wtimer);
    pthread_mutex_unlock(&timer_mu);
  }
  return atomic_load(&t->wait_res);
}

/* Waits through the poller; SD_WAIT_UNWATCHED if it cannot watch fd. */
static int wait_poller(sd_task *t, int fd, bool for_write, int64_t deadline) {
  pthread_once(&poller_once, poller_start);
  if (!poller_ok) return SD_WAIT_UNWATCHED;
#if defined(SD_KQUEUE)
  /* kqueue reports regular files only up to their end and is unreliable for
   * terminals and devices: those are polled in the OS. */
  struct stat st;
  if (fstat(fd, &st) != 0 || S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) return SD_WAIT_READY; /* poll would say so */
  if (!S_ISSOCK(st.st_mode) && !S_ISFIFO(st.st_mode)) return SD_WAIT_UNWATCHED;
#endif
  pthread_mutex_lock(&poll_mu);
  uint32_t idx;
  if (pslot_free != UINT32_MAX) {
    idx = pslot_free;
    pslot_free = pslots[idx].next_free;
  } else {
    if (npslots == cappslots) {
      cappslots = cappslots ? cappslots * 2 : 64;
      pslots = sd_realloc(pslots, cappslots * sizeof *pslots);
    }
    idx = npslots++;
    pslots[idx].gen = 0;
  }
  pslots[idx].task = t;
  uint32_t gen = ++pslots[idx].gen;
  pthread_mutex_unlock(&poll_mu);
  uint64_t token = (uint64_t)idx << 32 | gen;

  atomic_store(&t->wait_res, SD_WR_PENDING);
  wait_mode_set(t, SD_WM_PARK);
  int res = -1;
#if defined(SD_EPOLL)
  struct epoll_event ev;
  memset(&ev, 0, sizeof ev);
  ev.events = (for_write ? EPOLLOUT : EPOLLIN) | EPOLLONESHOT;
  ev.data.u64 = token;
  if (epoll_ctl(poll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
    int e = errno;
    /* A regular file or a device without polling (EPERM) or a bad
     * descriptor: poll() would report it ready (or invalid) at once. */
    res = (e == EPERM || e == EBADF) ? SD_WAIT_READY : SD_WAIT_UNWATCHED;
  }
#elif defined(SD_KQUEUE)
  struct kevent kev;
  EV_SET(&kev, (uintptr_t)fd, for_write ? EVFILT_WRITE : EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, (void *)(uintptr_t)token);
  if (kevent(poll_fd, &kev, 1, NULL, 0, NULL) != 0) res = SD_WAIT_UNWATCHED;
#endif
  if (res == -1) {
    res = wait_result(wait_park(t, deadline));
#if defined(SD_EPOLL)
    epoll_ctl(poll_fd, EPOLL_CTL_DEL, fd, &ev);
#elif defined(SD_KQUEUE)
    EV_SET(&kev, (uintptr_t)fd, for_write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(poll_fd, &kev, 1, NULL, 0, NULL);
#endif
  }
  pthread_mutex_lock(&poll_mu);
  pslots[idx].task = NULL;
  pslots[idx].gen++;
  pslots[idx].next_free = pslot_free;
  pslot_free = idx;
  pthread_mutex_unlock(&poll_mu);
  wait_mode_set(t, SD_WM_NONE);
  return res;
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

/* Waits for fd in poll(), in a block section, with the wake-up pipe. */
static int wait_os(sd_task *t, int fd, bool for_write, int64_t deadline, bool wakeable) {
  struct pollfd all[2];
  pthread_mutex_lock(&t->mu);
  wake_pipe_locked(t);
  t->wait_mode = SD_WM_PIPE;
  pthread_mutex_unlock(&t->mu);
  int wfd = t->wake_r, result, saved = 0;
  sd_block_begin();
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
    all[0] = (struct pollfd){fd, for_write ? POLLOUT : POLLIN, 0};
    int m = 1;
    if (wfd >= 0) all[m++] = (struct pollfd){wfd, POLLIN, 0};
    int r = poll(all, (nfds_t)m, ms);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      saved = errno;
      result = SD_WAIT_FAILED;
      break;
    }
    bool woken = wfd >= 0 && all[1].revents != 0;
    if (woken) {
      char buf[64];
      while (read(wfd, buf, sizeof buf) > 0) {
      }
    }
    if (atomic_load(&t->cancelled)) {
      result = SD_WAIT_STOP;
      break;
    }
    if (all[0].revents) {
      result = SD_WAIT_READY;
      break;
    }
    if (woken && wakeable) {
      result = SD_WAIT_WOKEN;
      break;
    }
  }
  sd_block_end();
  wait_mode_set(t, SD_WM_NONE);
  if (result == SD_WAIT_FAILED) errno = saved;
  return result;
}

/* Waits until fd is ready (for_write: writable), deadline passes (monotonic
 * ns; 0: none), the goroutine is stopped or (wakeable) woken by a close.
 * Returns SD_WAIT_READY, SD_WAIT_TIMEOUT, SD_WAIT_STOP, SD_WAIT_FAILED
 * (errno set) or SD_WAIT_WOKEN. */
static int wait_core(sd_task *t, int fd, bool for_write, int64_t deadline, bool wakeable) {
  for (;;) {
    if (atomic_load(&t->cancelled)) return SD_WAIT_STOP;
    if (deadline && deadline <= mono_ns()) return SD_WAIT_TIMEOUT;
    int r = wait_poller(t, fd, for_write, deadline);
    if (r == SD_WAIT_UNWATCHED) r = wait_os(t, fd, for_write, deadline, wakeable);
    if (r != SD_WAIT_WOKEN || wakeable) return r;
  }
}

int sd_wait_fd(int fd, bool for_write, int64_t timeout_ns) {
  return wait_core(task_current(), fd, for_write, timeout_ns > 0 ? mono_ns() + timeout_ns : 0, false);
}

bool sd_stopping(void) { return atomic_load(&task_current()->cancelled) != 0; }

void sd_stop(void) { stop_task(task_current()); }

typedef struct {
  pid_t pid;
  int status, err, fd;
} child_wait;

/* A helper thread does the blocking waitpid, then signals the pipe that the
 * goroutine waits on (POSIX has no descriptor for a child process). */
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
      sd_block_begin();
      pthread_join(th, NULL);
      sd_block_end();
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
  int err = 0;
  sd_block_begin();
  while (waitpid(pid, status, 0) < 0)
    if (errno != EINTR) {
      err = errno;
      break;
    }
  sd_block_end();
  return err;
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
  pthread_mutex_t mu; /* protects closed, busy, waiter, lockq, timeout */
  bool closed, busy;
  sd_task *waiter; /* the goroutine waiting for fd, woken by close */
  sd_task *lockq;  /* goroutines waiting for the operation lock (linked by hnext) */
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

/* With h->mu held: wakes the first goroutine waiting for the operation lock
 * (all of them with all). */
static void handle_lock_wake_locked(sd_handle *h, bool all) {
  while (h->lockq) {
    sd_task *w = h->lockq;
    h->lockq = w->hnext;
    w->hnext = NULL;
    pthread_mutex_lock(&w->mu);
    bool woke = false;
    if (w->wait_mode == SD_WM_PARK) {
      int z = SD_WR_PENDING;
      if (atomic_compare_exchange_strong(&w->wait_res, &z, SD_WR_WOKEN)) {
        unpark(w);
        woke = true;
      }
    }
    pthread_mutex_unlock(&w->mu);
    if (woke && !all) return;
  }
}

static void handle_lockq_remove_locked(sd_handle *h, sd_task *t) {
  for (sd_task **p = &h->lockq; *p; p = &(*p)->hnext) {
    if (*p == t) {
      *p = t->hnext;
      t->hnext = NULL;
      return;
    }
  }
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
    /* Another goroutine's operation on this handle: wait for it (a stop
     * point). */
    t->hnext = NULL;
    sd_task **tail = &h->lockq;
    while (*tail) tail = &(*tail)->hnext;
    *tail = t;
    atomic_store(&t->wait_res, SD_WR_PENDING);
    pthread_mutex_lock(&t->mu);
    t->wait_mode = SD_WM_PARK;
    pthread_mutex_unlock(&t->mu);
    pthread_mutex_unlock(&h->mu);
    int res = wait_park(t, 0);
    wait_mode_set(t, SD_WM_NONE);
    pthread_mutex_lock(&h->mu);
    handle_lockq_remove_locked(h, t);
    if (res == SD_WR_STOP) {
      if (!h->busy || h->closed) handle_lock_wake_locked(h, false); /* pass a wake-up it may have taken on */
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
  handle_lock_wake_locked(h, false);
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
    wait_wake_locked(h->waiter, SD_WR_WOKEN);
    pthread_mutex_unlock(&h->waiter->mu);
  }
  handle_lock_wake_locked(h, true);
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
    int r = wait_core(t, h->fd, for_write, deadline, true);
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
      /* A blocking descriptor (stdin, a file): read() only after poll() says
       * there is something, so that the wait stays a stop point. */
      int w = handle_wait(h, false, err);
      if (w != SD_IO_OK) return w;
      waited = true;
      sd_block_begin();
    }
    ssize_t r = read(h->fd, dst, n);
    int e = errno;
    if (!h->nonblock) sd_block_end();
    if (r > 0) {
      *got = (size_t)r;
      return SD_IO_OK;
    }
    if (r == 0) return SD_IO_EOF;
    if (e == EINTR) continue;
    if (e != EAGAIN && e != EWOULDBLOCK) {
      *err = e;
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
    if (!h->nonblock) sd_block_begin(); /* a file, or a pipe that may be full */
    ssize_t r = h->kind == SD_HANDLE_CONN ? send(h->fd, data + off, n - off, SD_MSG_NOSIGNAL) : write(h->fd, data + off, n - off);
    int e = errno;
    if (!h->nonblock) sd_block_end();
    if (r > 0) {
      off += (size_t)r;
      continue;
    }
    if (r < 0 && e == EINTR) continue;
    if (r < 0 && e != EAGAIN && e != EWOULDBLOCK) {
      *err = e;
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

#if defined(SD_LEAK_CHECK)
/* -fsanitize=leak (tests/leaks.sh on Linux): _exit skips LeakSanitizer's
 * own check at exit, so it runs here. */
void __lsan_do_leak_check(void);
#endif

void sd_exit_process(int code) {
  fflush(stdout);
  fflush(stderr);
#if defined(SD_LEAK_CHECK)
  __lsan_do_leak_check();
#endif
  _exit(code);
}

#if !defined(SD_NO_FAULT_HANDLER)
/* A memory fault (a stack overflow hitting the guard, say) crashes the
 * goroutine that runs on this thread, from the thread's alternate signal
 * stack. A fault in the scheduler itself (no goroutine) ends the program. */
static void fault_handler(int sig) {
  (void)sig;
  sd_task *t = sd_cur_;
  if (t && t->frame) {
    if (t->raise == SD_RAISE_NONE) {
      t->raise = SD_RAISE_CRASH;
      t->msg = (char *)"stack overflow or invalid memory access";
      t->msg_static = true;
    }
    lframes_release(t, t->frame->lmark);
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

static struct {
  sd_err (*fn)(sd_list *);
  int argc;
  char **argv;
} main_args;

typedef struct {
  sd_err (*fn)(sd_list *);
  sd_list *args;
  sd_err result;
} main_call;

static void run_main(void *p) {
  main_call *mc = p;
  mc->result = mc->fn(mc->args);
}

/* The main goroutine: Seed's main, then the end of the program. */
static SD_NORETURN void main_goroutine(sd_task *t) {
  sd_list *args = sd_list_new(&sd_type_str, main_args.argc);
  for (int i = 0; i < main_args.argc; i++) SD_LIST_PUSH(&args, sd_str, sd_str_from_cstr(main_args.argv[i]));
  main_call mc = {main_args.fn, args, SD_ERR_NULL};
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

/* Where every goroutine starts, on its own stack. */
static void g_entry(void) {
#if defined(SD_ASAN)
  const void *b;
  size_t s;
  __sanitizer_finish_switch_fiber(NULL, &b, &s);
  sd_m *m = tls_m();
  m->g0_bottom = b;
  m->g0_size = s;
#endif
  sd_task *t = tls_task();
  if (t == sd_main_task) main_goroutine(t);
  goroutine_main(t);
}

/* The main goroutine's stack: the stack limit (ulimit -s), as a main
 * thread would have, within 64 KB .. 1 GB (8 MB when unlimited). */
static size_t main_stack_size(void) {
  struct rlimit rl;
  size_t n = (size_t)8 << 20;
  if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) n = (size_t)rl.rlim_cur;
  if (n < ((size_t)64 << 10)) n = (size_t)64 << 10;
  if (n > ((size_t)1 << 30)) n = (size_t)1 << 30;
  return n;
}

static char sd_stdout_buf[1 << 16];

int sd_main(int argc, char **argv, sd_err (*main_fn)(sd_list *args)) {
  /* stdout is line-buffered on a terminal, so that output shows as it is
   * printed, and fully buffered otherwise; stderr is unbuffered. stdout is
   * also flushed before reading stdin, starting a child and exiting. */
  setvbuf(stdout, sd_stdout_buf, isatty(1) ? _IOLBF : _IOFBF, sizeof sd_stdout_buf);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_fault_handler();
  pthread_once(&sched_once, sched_init);
  main_args.fn = main_fn;
  main_args.argc = argc;
  main_args.argv = argv;
  /* The main thread becomes the first M; main runs as a goroutine on it. */
  sd_m *m = m_new();
  m_init_thread(m);
  sd_task *t = g_get(main_stack_size());
  sd_main_task = t;
  atomic_fetch_add(&sd_running, 1);
  ctx_prepare(t);
  pthread_mutex_lock(&sched.mu);
  sd_p *p = pidle_get_locked();
  pthread_mutex_unlock(&sched.mu);
  acquirep(m, p);
  runq_put(p, t, false);
  schedule(m);
}
