/* Natives of package os (runtime/README.md, "Native functions"). */
#include "seed.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

void sd_lib_os_exit(sd_int code) { sd_os_exit(code); }

void sd_lib_os_crash(sd_str message) { sd_os_crash(message); }

sd_str sd_lib_os_env(sd_str name) { return sd_os_env(name); }

sd_err sd_lib_os_run(sd_list *argv, sd_int *code) { return sd_os_run(argv, code); }

sd_str sd_lib_os_executable(void) { return sd_os_executable(); }

sd_err sd_lib_os_cwd(sd_str *out) { return sd_os_cwd(out); }

/* os.output: os.run with the child's stdout on a pipe. Both pipe ends are
 * close-on-exec, so no other child started at the same time inherits them;
 * dup2 onto the child's descriptor 1 clears the flag there. Reading waits
 * with sd_wait_fd and the end with sd_wait_child, so both are stop points:
 * a stopped goroutine kills and reaps the child. */
sd_err sd_lib_os_output(sd_list *argv, sd_str *out, sd_int *code) {
  if (argv->len == 0) sd_crash("os.output: argv is empty");
  sd_str const *xs = sd_list_data(argv);
  char **av = sd_alloc((size_t)(argv->len + 1) * sizeof(char *));
  for (int64_t i = 0; i < argv->len; i++) av[i] = (char *)xs[i]->data;
  av[argv->len] = NULL;
  int fds[2];
  if (pipe(fds) != 0) {
    sd_err e = sd_errorf("os.output: %s: %s", av[0], sd_strerror(errno));
    free(av);
    return e;
  }
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  fflush(stdout);
  fflush(stderr);
  pid_t pid;
  int r = posix_spawnp(&pid, av[0], &fa, NULL, av, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (r != 0) {
    close(fds[0]);
    sd_err e = sd_errorf("os.output: %s: %s", av[0], sd_strerror(r));
    free(av);
    return e;
  }
  int64_t cap = 4096, n = 0;
  char *buf = sd_alloc((size_t)cap);
  int readErr = 0;
  int st = 0;
  for (;;) {
    int w = sd_wait_fd(fds[0], false, 0);
    if (w == SD_WAIT_STOP) {
      close(fds[0]);
      free(buf);
      free(av);
      (void)sd_wait_child(pid, &st); /* the goroutine is stopped: kills and reaps the child */
      sd_stop();
    }
    if (w == SD_WAIT_FAILED) {
      if (errno == EINTR || errno == EAGAIN) continue;
      readErr = errno;
      break;
    }
    if (n == cap) {
      cap *= 2;
      buf = sd_realloc(buf, (size_t)cap);
    }
    ssize_t got = read(fds[0], buf + n, (size_t)(cap - n));
    if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (got < 0) readErr = errno;
    if (got <= 0) break;
    n += got;
  }
  close(fds[0]);
  int waitErr = sd_wait_child(pid, &st);
  if (waitErr == SD_WAIT_STOP) {
    free(buf);
    free(av);
    sd_stop();
  }
  if (readErr || waitErr) {
    sd_err e = sd_errorf("os.output: %s: %s", av[0], sd_strerror(readErr ? readErr : waitErr));
    free(buf);
    free(av);
    return e;
  }
  free(av);
  *out = sd_str_new(buf, n);
  free(buf);
  *code = SD_INT(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0));
  return SD_ERR_NULL;
}

sd_err sd_lib_os_hostname(sd_str *out) {
  char buf[1024];
  if (gethostname(buf, sizeof buf - 1) != 0) return sd_errorf("os.hostname: %s", sd_strerror(errno));
  buf[sizeof buf - 1] = 0;
  *out = sd_str_from_cstr(buf);
  return SD_ERR_NULL;
}
