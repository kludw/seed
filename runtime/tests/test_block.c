/* Blocking calls as stop points, and the handle table: cancelling goroutines
 * blocked in sleep, in poll on a pipe, in a handle read (non-blocking and
 * blocking descriptors), in accept, waiting for a child process and waiting
 * for another goroutine's operation on the same handle; close waking a
 * waiter; timeouts; buffered line reading; a large transfer over a socket
 * pair; finished goroutines freed while their group runs.
 * Run under -fsanitize=thread as well as address,undefined. */
#include "check.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static atomic_int defers_run;
static atomic_int reached_after_wait; /* a goroutine got past a wait that should have stopped it */

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
      fprintf(stderr, "test_block.c:%d: unexpected error: %s\n", line, sd_str_cstr(m));
      sd_str_release(m);
    }
  } else if (sd_err_is_null(e)) {
    check_failures++;
    fprintf(stderr, "test_block.c:%d: expected error \"%s\", got null\n", line, want);
  } else {
    sd_str m = sd_err_message(e);
    if (strcmp(sd_str_cstr(m), want) != 0) {
      check_failures++;
      fprintf(stderr, "test_block.c:%d: error \"%s\", want \"%s\"\n", line, sd_str_cstr(m), want);
    }
    sd_str_release(m);
  }
  sd_err_release(e);
}
#define CHECK_ERR(e, want) check_err((e), (want), __LINE__)

typedef struct { sd_chan *c; } A_chan;
static const sd_field F_chan[] = {SD_FIELD(A_chan, c, &sd_type_chan)};
static const sd_type T_chan = SD_STRUCT_TYPE("chan", A_chan, F_chan);

typedef struct { sd_int n; } A_n;
static const sd_field F_n[] = {SD_FIELD(A_n, n, &sd_type_int)};
static const sd_type T_n = SD_STRUCT_TYPE("n", A_n, F_n);

static int64_t arg_i64(void *p) {
  int64_t v = 0;
  sd_int_to_i64(((A_n *)p)->n, &v);
  return v;
}

static void count_defer(void *env) { atomic_fetch_add(&defers_run, 1); }

/* Fails the group after 50 ms, so that the other goroutine is blocked by
 * then (if it isn't yet, it stops at the start of its wait instead). */
static sd_err failer(void *p) {
  sd_time_sleep(SD_INT(50000000));
  return sd_errorf("fail");
}

/* Runs body(n) next to failer in a group; checks that the group fails with
 * failer's error quickly, that body's defer ran and that it never got past
 * its wait. */
static void run_cancel(const char *what, sd_task_fn body, int64_t n) {
  atomic_store(&defers_run, 0);
  atomic_store(&reached_after_wait, 0);
  double t0 = now_s();
  sd_group *g = sd_group_begin();
  A_n a = {sd_int_from_i64(n)};
  sd_spawn(g, body, &T_n, &a);
  sd_spawn(g, failer, NULL, NULL);
  CHECK_ERR(sd_group_wait(g), "fail");
  double dt = now_s() - t0;
  check_count++;
  if (dt > 3.0) {
    check_failures++;
    fprintf(stderr, "test_block.c: cancelling a goroutine blocked in %s took %.2fs\n", what, dt);
  }
  CHECK(atomic_load(&defers_run) == 1);
  CHECK(atomic_load(&reached_after_wait) == 0);
}

/* ---- the goroutines that block ---- */

static sd_err blocked_sleep(void *p) {
  sd_defer_push(count_defer, NULL, 0);
  sd_time_sleep(SD_INT(30000000000));
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

static sd_err blocked_wait_fd(void *p) {
  sd_defer_push(count_defer, NULL, 0);
  int r = sd_wait_fd((int)arg_i64(p), false, 30000000000);
  if (r == SD_WAIT_STOP) sd_stop();
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

/* Reads from handle n (a FILE handle). */
static sd_err blocked_handle_read(void *p) {
  sd_defer_push(count_defer, NULL, 0);
  sd_handle *h = sd_handle_acquire(arg_i64(p), SD_HANDLE_FILE);
  if (!h) return sd_errorf("no handle");
  sd_str s;
  int err = 0;
  sd_handle_read(h, 10, &s, &err); /* stops, releasing h */
  sd_str_release(s);
  sd_handle_release(h);
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

static sd_err blocked_handle_line(void *p) {
  sd_defer_push(count_defer, NULL, 0);
  sd_handle *h = sd_handle_acquire(arg_i64(p), SD_HANDLE_STDIN);
  if (!h) return sd_errorf("no handle");
  sd_str s;
  int err = 0;
  sd_handle_read_line(h, &s, &err);
  sd_str_release(s);
  sd_handle_release(h);
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

static sd_err blocked_accept(void *p) {
  sd_defer_push(count_defer, NULL, 0);
  sd_handle *h = sd_handle_acquire(arg_i64(p), SD_HANDLE_LISTENER);
  if (!h) return sd_errorf("no handle");
  sd_handle_wait(h, false);
  sd_handle_release(h);
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

typedef struct { sd_lframe h; sd_list *argv; } L_argv;
static void lr_argv(sd_lframe *p) { sd_list_release(((L_argv *)p)->argv); }

static sd_err blocked_child(void *p) {
  sd_defer_push(count_defer, NULL, 0);
  L_argv L = {{0}};
  SD_LF_ENTER(L, lr_argv);
  L.argv = sd_list_new(&sd_type_str, 2);
  SD_LIST_PUSH(&L.argv, sd_str, sd_str_from_cstr("sleep"));
  SD_LIST_PUSH(&L.argv, sd_str, sd_str_from_cstr("30"));
  sd_int code;
  sd_err e = sd_os_run(L.argv, &code); /* stops; the frame releases argv */
  SD_LF_LEAVE(L);
  sd_err_release(e);
  sd_list_release(L.argv);
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

/* Two goroutines on one handle (id, with a channel between them): the
 * first is blocked in a read holding the operation lock, the second waits
 * for that lock. */
typedef struct { sd_int id; sd_chan *c; } A_id_chan;
static const sd_field F_id_chan[] = {SD_FIELD(A_id_chan, id, &sd_type_int), SD_FIELD(A_id_chan, c, &sd_type_chan)};
static const sd_type T_id_chan = SD_STRUCT_TYPE("id_chan", A_id_chan, F_id_chan);

static sd_err holds_lock(void *p) {
  A_id_chan *a = p;
  sd_defer_push(count_defer, NULL, 0);
  int64_t id = 0;
  sd_int_to_i64(a->id, &id);
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_FILE);
  if (!h) return sd_errorf("no handle");
  sd_int one = SD_INT(1);
  sd_chan_send(a->c, &one); /* buffered */
  sd_str s;
  int err = 0;
  sd_handle_read(h, 10, &s, &err);
  sd_str_release(s);
  sd_handle_release(h);
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

static sd_err waits_for_lock(void *p) {
  A_id_chan *a = p;
  sd_defer_push(count_defer, NULL, 0);
  int64_t id = 0;
  sd_int_to_i64(a->id, &id);
  sd_int v;
  if (!sd_chan_receive(a->c, &v)) return sd_errorf("closed");
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_FILE);
  if (h) sd_handle_release(h);
  atomic_store(&reached_after_wait, 1);
  return SD_ERR_NULL;
}

static void make_pipe(int p[2], bool nonblock) {
  if (pipe(p) != 0) {
    perror("pipe");
    exit(1);
  }
  if (nonblock) {
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);
  }
}

static void test_cancel_blocked(void) {
  run_cancel("sleep", blocked_sleep, 0);

  int p[2];
  make_pipe(p, false);
  run_cancel("poll on a pipe", blocked_wait_fd, p[0]);
  close(p[1]);
  close(p[0]);

  make_pipe(p, true);
  int64_t id = sd_handle_new(SD_HANDLE_FILE, p[0], "pipe");
  run_cancel("a handle read (non-blocking descriptor)", blocked_handle_read, id);
  /* The stopped read released the handle: it can be used again. */
  CHECK(write(p[1], "hi", 2) == 2);
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_FILE);
  CHECK(h != NULL);
  if (h) {
    sd_str s;
    int err = 0;
    CHECK(sd_handle_read(h, 10, &s, &err) == SD_IO_OK);
    CHECK_SDSTR(s, "hi");
    sd_handle_release(h);
  }

  /* Two goroutines on one handle: one blocked in read, one waiting for the
   * operation lock. Both stop. */
  atomic_store(&defers_run, 0);
  atomic_store(&reached_after_wait, 0);
  sd_group *g = sd_group_begin();
  A_id_chan a = {sd_int_from_i64(id), sd_chan_new(&sd_type_int, SD_INT(1))};
  sd_spawn(g, holds_lock, &T_id_chan, &a);
  sd_spawn(g, waits_for_lock, &T_id_chan, &a);
  sd_spawn(g, failer, NULL, NULL);
  CHECK_ERR(sd_group_wait(g), "fail");
  CHECK(atomic_load(&defers_run) == 2);
  CHECK(atomic_load(&reached_after_wait) == 0);
  sd_chan_release(a.c);
  CHECK(sd_handle_close(id));
  CHECK(!sd_handle_close(id));
  close(p[1]);

  make_pipe(p, false);
  id = sd_handle_new(SD_HANDLE_STDIN, p[0], "");
  run_cancel("a line read (blocking descriptor)", blocked_handle_line, id);
  CHECK(sd_handle_close(id));
  close(p[0]); /* STDIN handles don't close their descriptor */
  close(p[1]);

  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(s, (struct sockaddr *)&sa, sizeof sa) == 0);
  CHECK(listen(s, 8) == 0);
  fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
  id = sd_handle_new(SD_HANDLE_LISTENER, s, "127.0.0.1:0");
  run_cancel("accept", blocked_accept, id);
  CHECK(sd_handle_close(id));

  run_cancel("waiting for a child process", blocked_child, 0);
}

/* ---- close wakes a waiter; timeouts ---- */

typedef struct { sd_int id; sd_chan *out; } A_id_out;
static const sd_field F_id_out[] = {SD_FIELD(A_id_out, id, &sd_type_int), SD_FIELD(A_id_out, out, &sd_type_chan)};
static const sd_type T_id_out = SD_STRUCT_TYPE("id_out", A_id_out, F_id_out);

/* Reads handle id and sends the result code on out. */
static sd_err reader(void *p) {
  A_id_out *a = p;
  int64_t id = 0;
  sd_int_to_i64(a->id, &id);
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_CONN);
  int r = SD_IO_CLOSED;
  if (h) {
    sd_str s;
    int err = 0;
    r = sd_handle_read(h, 100, &s, &err);
    sd_str_release(s);
    if (r != SD_IO_OK) {
      sd_err e = sd_handle_error("net.Conn.read", h, SD_HANDLE_CONN, r, err);
      CHECK_ERR(e, r == SD_IO_TIMEOUT ? "net.Conn.read: peer: timeout" : "net.Conn.read: peer: connection is closed");
    }
    sd_handle_release(h);
  }
  sd_int v = SD_INT(r);
  sd_chan_send(a->out, &v);
  return SD_ERR_NULL;
}

static sd_err closer(void *p) {
  int64_t id = arg_i64(p);
  sd_time_sleep(SD_INT(50000000));
  sd_handle_close(id);
  return SD_ERR_NULL;
}

static void test_close_and_timeout(void) {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
  int64_t id = sd_handle_new(SD_HANDLE_CONN, sv[0], "peer");
  sd_chan *out = sd_chan_new(&sd_type_int, SD_INT(1));

  /* Timeout: nothing arrives within 30 ms; the handle stays usable. */
  sd_handle_set_timeout(id, 30000000);
  double t0 = now_s();
  sd_group *g = sd_group_begin();
  A_id_out a = {sd_int_from_i64(id), out};
  sd_spawn(g, reader, &T_id_out, &a);
  CHECK_ERR(sd_group_wait(g), NULL);
  sd_int v;
  CHECK(sd_chan_receive(out, &v) && sd_int_eq(v, SD_INT(SD_IO_TIMEOUT)));
  CHECK(now_s() - t0 >= 0.025);
  sd_handle_set_timeout(id, 0);

  /* Close from another goroutine wakes the reader. */
  g = sd_group_begin();
  sd_spawn(g, reader, &T_id_out, &a);
  A_n an = {sd_int_from_i64(id)};
  sd_spawn(g, closer, &T_n, &an);
  CHECK_ERR(sd_group_wait(g), NULL);
  CHECK(sd_chan_receive(out, &v) && sd_int_eq(v, SD_INT(SD_IO_CLOSED)));
  CHECK(sd_handle_acquire(id, SD_HANDLE_CONN) == NULL);
  CHECK_SDSTR(sd_handle_subject(id), "");
  CHECK_ERR(sd_handle_error("net.Conn.write", NULL, SD_HANDLE_CONN, SD_IO_CLOSED, 0), "net.Conn.write: connection is closed");
  CHECK_ERR(sd_handle_error("net.Listener.accept", NULL, SD_HANDLE_LISTENER, SD_IO_CLOSED, 0),
            "net.Listener.accept: listener is closed");
  /* The descriptor was closed: the other end sees end of input. */
  char c;
  CHECK(read(sv[1], &c, 1) == 0);
  close(sv[1]);
  sd_chan_release(out);
}

/* ---- buffered reading ---- */

typedef struct {
  int fd;
  const char *data;
  size_t n;
} writer_args;

static void *writer(void *p) {
  writer_args *w = p;
  size_t off = 0;
  while (off < w->n) {
    ssize_t r = write(w->fd, w->data + off, w->n - off);
    if (r <= 0) break;
    off += (size_t)r;
  }
  close(w->fd);
  return NULL;
}

static void test_lines(void) {
  int p[2];
  make_pipe(p, false);
  const char *text = "a\r\nb\n\nc\rd\nlast";
  CHECK(write(p[1], text, strlen(text)) == (ssize_t)strlen(text));
  close(p[1]);
  int64_t id = sd_handle_new(SD_HANDLE_STDIN, p[0], "");
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_STDIN);
  CHECK(h != NULL);
  if (!h) return;
  CHECK(sd_handle_acquire(id, SD_HANDLE_CONN) == NULL); /* wrong kind */
  const char *want[] = {"a", "b", "", "c\rd", "last"};
  int err = 0;
  sd_str s;
  for (int i = 0; i < 5; i++) {
    CHECK(sd_handle_read_line(h, &s, &err) == SD_IO_OK);
    if (s) CHECK_SDSTR(s, want[i]);
  }
  CHECK(sd_handle_read_line(h, &s, &err) == SD_IO_EOF);
  CHECK(s == NULL);
  CHECK(sd_handle_read_all(h, &s, &err) == SD_IO_OK);
  CHECK_SDSTR(s, "");
  sd_handle_release(h);
  CHECK(sd_handle_close(id));
  close(p[0]);

  /* A line longer than the buffer, then readAll for the rest. The data is
   * written from a thread: the pipe holds less than the line. */
  make_pipe(p, false);
  size_t big = 200000;
  char *data = malloc(big + 8);
  memset(data, 'x', big);
  memcpy(data + big, "\nrest\n", 6);
  writer_args wa = {p[1], data, big + 6};
  pthread_t th;
  pthread_create(&th, NULL, writer, &wa);
  id = sd_handle_new(SD_HANDLE_STDIN, p[0], "");
  h = sd_handle_acquire(id, SD_HANDLE_STDIN);
  CHECK(sd_handle_read_line(h, &s, &err) == SD_IO_OK);
  CHECK(s && s->len == (int64_t)big && s->data[0] == 'x' && s->data[big - 1] == 'x');
  sd_str_release(s);
  CHECK(sd_handle_read_all(h, &s, &err) == SD_IO_OK);
  CHECK_SDSTR(s, "rest\n");
  sd_handle_release(h);
  sd_handle_close(id);
  pthread_join(th, NULL);
  close(p[0]);
  free(data);
}

/* ---- a large transfer over a socket pair, in two goroutines ---- */

static sd_err big_writer(void *p) {
  int64_t id = arg_i64(p);
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_CONN);
  if (!h) return sd_errorf("no handle");
  size_t n = 3 << 20;
  char *data = malloc(n);
  for (size_t i = 0; i < n; i++) data[i] = (char)(i * 7 % 251);
  int err = 0;
  int r = sd_handle_write(h, data, (int64_t)n, &err);
  free(data);
  sd_handle_release(h);
  sd_handle_close(id);
  return r == SD_IO_OK ? SD_ERR_NULL : sd_errorf("write failed");
}

static sd_err big_reader(void *p) {
  int64_t id = arg_i64(p);
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_CONN);
  if (!h) return sd_errorf("no handle");
  size_t total = 0;
  bool same = true;
  for (;;) {
    sd_str s;
    int err = 0;
    int r = sd_handle_read(h, 100000, &s, &err);
    if (r == SD_IO_EOF) break;
    if (r != SD_IO_OK) {
      sd_handle_release(h);
      return sd_errorf("read failed");
    }
    for (int64_t i = 0; i < s->len; i++)
      if (s->data[i] != (char)((total + (size_t)i) * 7 % 251)) same = false;
    total += (size_t)s->len;
    sd_str_release(s);
  }
  sd_handle_release(h);
  sd_handle_close(id);
  if (total != (size_t)3 << 20 || !same) return sd_errorf("got %zu bytes", total);
  return SD_ERR_NULL;
}

static void test_transfer(void) {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL) | O_NONBLOCK);
  A_n a = {sd_int_from_i64(sd_handle_new(SD_HANDLE_CONN, sv[0], "a"))};
  A_n b = {sd_int_from_i64(sd_handle_new(SD_HANDLE_CONN, sv[1], "b"))};
  sd_group *g = sd_group_begin();
  sd_spawn(g, big_writer, &T_n, &a);
  sd_spawn(g, big_reader, &T_n, &b);
  CHECK_ERR(sd_group_wait(g), NULL);
}

/* ---- finished goroutines are freed while their group runs ---- */

static sd_err quick(void *p) {
  sd_int v = SD_INT(1);
  sd_chan_send(((A_chan *)p)->c, &v);
  return SD_ERR_NULL;
}

static void test_reap(int n) {
  /* One group spawns n goroutines one after another, each finishing before
   * the next starts. Without reaping, n unjoined threads would pile up. */
  sd_chan *c = sd_chan_new(&sd_type_int, SD_INT(0));
  sd_group *g = sd_group_begin();
  A_chan a = {c};
  int got = 0;
  for (int i = 0; i < n; i++) {
    sd_spawn(g, quick, &T_chan, &a);
    sd_int v;
    if (sd_chan_receive(c, &v)) got++;
  }
  CHECK_ERR(sd_group_wait(g), NULL);
  CHECK(got == n);
  sd_chan_release(c);
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 2000;
  test_cancel_blocked();
  test_close_and_timeout();
  test_lines();
  test_transfer();
  test_reap(n);
  return check_done("test_block");
}
