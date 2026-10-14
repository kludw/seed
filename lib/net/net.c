/* Natives of package net (runtime/README.md, "Native functions"). Sockets
 * are non-blocking descriptors in the runtime's handle table; every wait
 * goes through the runtime (sd_handle_wait, sd_wait_fd), which makes it a
 * stop point. */
#include "seed.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Close-on-exec, non-blocking, and no SIGPIPE where the OS has a socket
 * option for it (Linux uses MSG_NOSIGNAL on each send instead). */
static void net_setup_fd(int fd, bool tcp) {
  fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  int one = 1;
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  if (tcp) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* "host:port" or "[v6]:port" text of a socket address. */
static void net_format(const struct sockaddr *sa, socklen_t len, char *buf, size_t n) {
  char host[NI_MAXHOST], port[NI_MAXSERV];
  if (getnameinfo(sa, len, host, sizeof host, port, sizeof port, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    snprintf(buf, n, "?");
    return;
  }
  if (sa->sa_family == AF_INET6) snprintf(buf, n, "[%s]:%s", host, port);
  else snprintf(buf, n, "%s:%s", host, port);
}

/* Splits "host:port" (host may be empty or "[v6]") into malloc'ed host
 * (without brackets) and port. Returns NULL, or the reason it is invalid. */
static const char *net_split(sd_str addr, char **host, char **port) {
  *host = *port = NULL;
  const char *s = addr->data;
  if ((int64_t)strlen(s) != addr->len) return "invalid address";
  const char *colon;
  const char *h0 = s, *h1;
  if (s[0] == '[') {
    const char *close = strchr(s, ']');
    if (!close) return "missing ']' in address";
    h0 = s + 1;
    h1 = close;
    if (close[1] != ':') return close[1] ? "invalid address" : "missing port";
    colon = close + 1;
  } else {
    colon = strrchr(s, ':');
    if (!colon) return "missing port";
    if (memchr(s, ':', (size_t)(colon - s))) return "IPv6 addresses need brackets: [::1]:80";
    h1 = colon;
  }
  const char *p = colon + 1;
  if (!*p || strlen(p) > 5) return *p ? "invalid port" : "missing port";
  long v = 0;
  for (const char *q = p; *q; q++) {
    if (*q < '0' || *q > '9') return "invalid port";
    v = v * 10 + (*q - '0');
  }
  if (v > 65535) return "invalid port";
  *host = malloc((size_t)(h1 - h0) + 1);
  memcpy(*host, h0, (size_t)(h1 - h0));
  (*host)[h1 - h0] = 0;
  *port = malloc(strlen(p) + 1);
  strcpy(*port, p);
  return NULL;
}

static const char *net_gai_reason(int rc) {
  static _Thread_local char buf[256];
  snprintf(buf, sizeof buf, "%s", rc == EAI_SYSTEM ? sd_strerror(errno) : gai_strerror(rc));
  if (buf[0] >= 'A' && buf[0] <= 'Z' && !(buf[1] >= 'A' && buf[1] <= 'Z')) buf[0] = (char)(buf[0] - 'A' + 'a');
  return buf;
}

static int64_t net_mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ------------------------------------------------------------- listen */

sd_err sd_lib_net_listen(sd_str addr, sd_S_net_Listener *out) {
  char *host, *port;
  const char *bad = net_split(addr, &host, &port);
  if (bad) return sd_errorf("net.listen: %s: %s", addr->data, bad);
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if (!host[0]) hints.ai_family = AF_INET; /* ":8080": every IPv4 interface */
  int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
  free(host);
  free(port);
  if (rc != 0) return sd_errorf("net.listen: %s: %s", addr->data, net_gai_reason(rc));
  int last = EADDRNOTAVAIL, fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      last = errno;
      continue;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* The longest accept queue the OS allows: it clamps the backlog to its
     * own limit (kern.ipc.somaxconn on macOS and the BSDs, 128 by default;
     * net.core.somaxconn on Linux, 4096 on recent kernels), as Go does. */
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 65535) == 0) break;
    last = errno;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return sd_errorf("net.listen: %s: %s", addr->data, sd_strerror(last));
  net_setup_fd(fd, false);
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  char name[NI_MAXHOST + NI_MAXSERV + 4] = "";
  if (getsockname(fd, (struct sockaddr *)&ss, &len) == 0) net_format((struct sockaddr *)&ss, len, name, sizeof name);
  out->f_id = sd_int_from_i64(sd_handle_new(SD_HANDLE_LISTENER, fd, name));
  return SD_ERR_NULL;
}

/* A connection handle for a connected socket, named after its peer. */
static int64_t net_conn_new(int fd) {
  net_setup_fd(fd, true);
  struct sockaddr_storage ss;
  socklen_t len = sizeof ss;
  char name[NI_MAXHOST + NI_MAXSERV + 4] = "";
  if (getpeername(fd, (struct sockaddr *)&ss, &len) == 0) net_format((struct sockaddr *)&ss, len, name, sizeof name);
  return sd_handle_new(SD_HANDLE_CONN, fd, name);
}

sd_err sd_lib_net_Listener_accept(sd_S_net_Listener l, sd_S_net_Conn *out) {
  int64_t id = 0;
  sd_int_to_i64(l.f_id, &id);
  sd_handle *h = sd_handle_acquire(id, SD_HANDLE_LISTENER);
  if (!h) return sd_handle_error("net.Listener.accept", NULL, SD_HANDLE_LISTENER, SD_IO_CLOSED, 0);
  for (;;) {
    int fd = accept(sd_handle_fd(h), NULL, NULL);
    if (fd >= 0) {
      sd_handle_release(h);
      out->f_id = sd_int_from_i64(net_conn_new(fd));
      return SD_ERR_NULL;
    }
    int e = errno;
    if (e == EINTR || e == ECONNABORTED || e == EPROTO) continue;
    int r = SD_IO_ERROR;
    if (e == EAGAIN || e == EWOULDBLOCK) {
      r = sd_handle_wait(h, false);
      if (r == SD_IO_OK) continue;
      e = errno;
    }
    sd_err err = sd_handle_error("net.Listener.accept", h, SD_HANDLE_LISTENER, r, e);
    sd_handle_release(h);
    return err;
  }
}

sd_str sd_lib_net_Listener_addr(sd_S_net_Listener l) {
  int64_t id = 0;
  sd_int_to_i64(l.f_id, &id);
  return sd_handle_subject(id);
}

void sd_lib_net_Listener_close(sd_S_net_Listener l) {
  int64_t id = 0;
  sd_int_to_i64(l.f_id, &id);
  sd_handle_close(id);
}

/* --------------------------------------------------------------- dial */

/* Name resolution runs in a helper thread, so that it can be abandoned when
 * the dial times out or the goroutine is stopped. Whichever side finishes
 * last frees the request. */
typedef struct {
  char *host, *port;
  struct addrinfo *res;
  int rc, err, fd_r, fd_w;
  atomic_int refs, done; /* done: the results are written (the pipe wakes the waiter; this orders the memory) */
} net_lookup;

static void net_lookup_unref(net_lookup *q) {
  if (atomic_fetch_sub(&q->refs, 1) != 1) return;
  if (q->res) freeaddrinfo(q->res);
  close(q->fd_r);
  close(q->fd_w);
  free(q->host);
  free(q->port);
  free(q);
}

static void *net_lookup_main(void *p) {
  net_lookup *q = p;
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;
  q->rc = getaddrinfo(q->host, q->port, &hints, &q->res);
  q->err = errno;
  atomic_store_explicit(&q->done, 1, memory_order_release);
  char c = 1;
  ssize_t w = write(q->fd_w, &c, 1);
  (void)w;
  net_lookup_unref(q);
  return NULL;
}

/* Resolves host:port within deadline (0: none). Returns 0 with *res set, an
 * EAI_ code (with errno for EAI_SYSTEM), or SD_WAIT_TIMEOUT / SD_WAIT_STOP
 * as negative values -1000 - x. */
#define NET_TIMEOUT (-1000)
#define NET_STOP (-1001)
static int net_resolve(char *host, char *port, int64_t deadline, struct addrinfo **res) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  int rc = getaddrinfo(host, port, &hints, res);
  if (rc != EAI_NONAME) return rc; /* a numeric address: no lookup needed */
  int p[2];
  if (pipe(p) != 0) return EAI_SYSTEM;
  net_lookup *q = calloc(1, sizeof *q);
  q->host = host;
  q->port = port;
  q->fd_r = p[0];
  q->fd_w = p[1];
  fcntl(p[0], F_SETFD, FD_CLOEXEC);
  fcntl(p[1], F_SETFD, FD_CLOEXEC);
  atomic_init(&q->refs, 2);
  atomic_init(&q->done, 0);
  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&th, &attr, net_lookup_main, q) != 0) {
    pthread_attr_destroy(&attr);
    q->host = q->port = NULL;
    atomic_store(&q->refs, 1);
    net_lookup_unref(q);
    errno = EAGAIN;
    return EAI_SYSTEM;
  }
  pthread_attr_destroy(&attr);
  int w;
  do {
    int64_t left = 0;
    if (deadline) {
      left = deadline - net_mono_ns();
      if (left <= 0) {
        w = SD_WAIT_TIMEOUT;
        break;
      }
    }
    w = sd_wait_fd(q->fd_r, false, left);
  } while (w == SD_WAIT_FAILED);
  if (w != SD_WAIT_READY) {
    net_lookup_unref(q); /* the thread frees the rest when the lookup ends */
    return w == SD_WAIT_STOP ? NET_STOP : NET_TIMEOUT;
  }
  while (!atomic_load_explicit(&q->done, memory_order_acquire)) {
  }
  rc = q->rc;
  errno = q->err;
  *res = q->res;
  q->res = NULL;
  q->host = q->port = NULL; /* the caller's */
  net_lookup_unref(q);
  return rc;
}

sd_err sd_lib_net_dial(sd_str addr, sd_int timeout, sd_S_net_Conn *out) {
  int64_t t = 0;
  if (!sd_int_to_i64(timeout, &t)) t = sd_int_lt(timeout, SD_INT(0)) ? -1 : INT64_MAX / 4;
  if (t < 0) {
    sd_crash_int("net.dial: timeout (%s) must not be negative", timeout);
  }
  if (t > INT64_MAX / 4) t = INT64_MAX / 4;
  int64_t deadline = t > 0 ? net_mono_ns() + t : 0;
  char *host, *port;
  const char *bad = net_split(addr, &host, &port);
  if (bad) return sd_errorf("net.dial: %s: %s", addr->data, bad);
  if (!host[0]) {
    free(host);
    host = malloc(10);
    strcpy(host, "localhost");
  }
  struct addrinfo *res = NULL;
  int rc = net_resolve(host, port, deadline, &res);
  int saved = errno;
  if (rc == NET_STOP || rc == NET_TIMEOUT) {
    /* host and port belong to the abandoned lookup now */
    if (rc == NET_STOP) sd_stop();
    return sd_errorf("net.dial: %s: timeout", addr->data);
  }
  free(host);
  free(port);
  if (rc != 0) {
    errno = saved;
    return sd_errorf("net.dial: %s: %s", addr->data, net_gai_reason(rc));
  }
  int last = ECONNREFUSED;
  bool timed_out = false;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      last = errno;
      continue;
    }
    net_setup_fd(fd, true);
    int r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r != 0 && errno == EINPROGRESS) {
      int64_t left = 0;
      int w = SD_WAIT_TIMEOUT;
      if (deadline) left = deadline - net_mono_ns();
      if (!deadline || left > 0) {
        do w = sd_wait_fd(fd, true, deadline ? left : 0);
        while (w == SD_WAIT_FAILED && errno == EINTR);
      }
      if (w == SD_WAIT_STOP) {
        close(fd);
        freeaddrinfo(res);
        sd_stop();
      }
      if (w == SD_WAIT_TIMEOUT) {
        close(fd);
        timed_out = true;
        break;
      }
      int soerr = 0;
      socklen_t len = sizeof soerr;
      if (w == SD_WAIT_FAILED) soerr = errno;
      else if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0) soerr = errno;
      r = soerr ? -1 : 0;
      errno = soerr;
    }
    if (r == 0) {
      freeaddrinfo(res);
      out->f_id = sd_int_from_i64(net_conn_new(fd));
      return SD_ERR_NULL;
    }
    last = errno;
    close(fd);
  }
  freeaddrinfo(res);
  if (timed_out) return sd_errorf("net.dial: %s: timeout", addr->data);
  return sd_errorf("net.dial: %s: %s", addr->data, sd_strerror(last));
}

/* --------------------------------------------------------------- Conn */

static int64_t net_id(sd_int v) {
  int64_t id = 0;
  sd_int_to_i64(v, &id);
  return id;
}

sd_err sd_lib_net_Conn_read(sd_S_net_Conn c, sd_int max, sd_str *out) {
  int64_t m = 0;
  if (!sd_int_to_i64(max, &m)) m = sd_int_lt(max, SD_INT(0)) ? 0 : INT64_MAX;
  if (m < 1) {
    sd_crash_int("net.Conn.read: max (%s) must be at least 1", max);
  }
  sd_handle *h = sd_handle_acquire(net_id(c.f_id), SD_HANDLE_CONN);
  if (!h) return sd_handle_error("net.Conn.read", NULL, SD_HANDLE_CONN, SD_IO_CLOSED, 0);
  int err = 0;
  int r = sd_handle_read(h, m, out, &err);
  sd_err e = SD_ERR_NULL;
  if (r != SD_IO_OK && r != SD_IO_EOF) e = sd_handle_error("net.Conn.read", h, SD_HANDLE_CONN, r, err);
  sd_handle_release(h);
  return e;
}

sd_err sd_lib_net_Conn_write(sd_S_net_Conn c, sd_str data) {
  sd_handle *h = sd_handle_acquire(net_id(c.f_id), SD_HANDLE_CONN);
  if (!h) return sd_handle_error("net.Conn.write", NULL, SD_HANDLE_CONN, SD_IO_CLOSED, 0);
  int err = 0;
  int r = sd_handle_write(h, data->data, data->len, &err);
  sd_err e = SD_ERR_NULL;
  if (r != SD_IO_OK) e = sd_handle_error("net.Conn.write", h, SD_HANDLE_CONN, r, err);
  sd_handle_release(h);
  return e;
}

void sd_lib_net_Conn_setTimeout(sd_S_net_Conn c, sd_int d) {
  int64_t ns = 0;
  if (!sd_int_to_i64(d, &ns)) ns = sd_int_lt(d, SD_INT(0)) ? -1 : INT64_MAX / 4;
  if (ns < 0) {
    sd_crash_int("net.Conn.setTimeout: d (%s) must not be negative", d);
  }
  if (ns > INT64_MAX / 4) ns = INT64_MAX / 4;
  sd_handle_set_timeout(net_id(c.f_id), ns);
}

sd_str sd_lib_net_Conn_remoteAddr(sd_S_net_Conn c) { return sd_handle_subject(net_id(c.f_id)); }

void sd_lib_net_Conn_close(sd_S_net_Conn c) { sd_handle_close(net_id(c.f_id)); }
