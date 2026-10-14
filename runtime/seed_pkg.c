/* The OS side of the core packages fmt, os and fs (errors is in
 * seed_core.c, time in seed_task.c). The packages themselves are in lib/;
 * their native functions (lib/<pkg>/<pkg>.c) call these. Errors follow
 * docs/stdlib.md: "<package>.<function>: <subject>: <reason>". */
#include "seed_internal.h"

#include <dirent.h>
#include <limits.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* The text is in a per-thread buffer, read by the caller before it can park
 * (noinline: the buffer's address is computed on every call). */
SD_NOINLINE const char *sd_strerror(int e) {
  static _Thread_local char buf[256];
  snprintf(buf, sizeof buf, "%s", strerror(e));
  if (buf[0] >= 'A' && buf[0] <= 'Z' && !(buf[1] >= 'A' && buf[1] <= 'Z')) buf[0] = (char)(buf[0] - 'A' + 'a');
  return buf;
}

/* ------------------------------------------------------------------ fmt
 *
 * Calls that may block in the OS (stdout into a full pipe, the file
 * system, starting a program) run in block sections (seed.h), so that the
 * other goroutines keep running meanwhile. */

void sd_fmt_print(sd_str s) {
  sd_block_begin();
  flockfile(stdout);
  fwrite(s->data, 1, (size_t)s->len, stdout);
  funlockfile(stdout);
  sd_block_end();
}

void sd_fmt_println(sd_str s) {
  sd_block_begin();
  flockfile(stdout);
  fwrite(s->data, 1, (size_t)s->len, stdout);
  putc_unlocked('\n', stdout);
  funlockfile(stdout);
  sd_block_end();
}

void sd_fmt_eprint(sd_str s) {
  sd_block_begin();
  fflush(stdout);
  flockfile(stderr);
  fwrite(s->data, 1, (size_t)s->len, stderr);
  funlockfile(stderr);
  sd_block_end();
}

void sd_fmt_eprintln(sd_str s) {
  sd_block_begin();
  fflush(stdout);
  flockfile(stderr);
  fwrite(s->data, 1, (size_t)s->len, stderr);
  putc_unlocked('\n', stderr);
  funlockfile(stderr);
  sd_block_end();
}

/* ------------------------------------------------------------------- os */

void sd_os_exit(sd_int code) {
  int64_t c;
  if (!sd_int_to_i64(code, &c)) c = 255;
  sd_exit_process((int)(c & 0xff));
}

sd_str sd_os_env(sd_str name) {
  if ((int64_t)strlen(name->data) != name->len) return NULL;
  const char *v = getenv(name->data);
  return v ? sd_str_from_cstr(v) : NULL;
}

SD_NORETURN void sd_os_crash(sd_str message) { sd_crash("%s", message->data); }

sd_str sd_os_executable(void) {
  char raw[PATH_MAX], real[PATH_MAX];
  raw[0] = 0;
#ifdef __APPLE__
  uint32_t size = sizeof raw;
  if (_NSGetExecutablePath(raw, &size) != 0) raw[0] = 0;
#else
  ssize_t n = readlink("/proc/self/exe", raw, sizeof raw - 1);
  raw[n > 0 ? n : 0] = 0;
#endif
  if (raw[0] && realpath(raw, real)) return sd_str_from_cstr(real);
  return sd_str_from_cstr(raw);
}

sd_err sd_os_cwd(sd_str *out) {
  char buf[PATH_MAX];
  *out = NULL;
  if (!getcwd(buf, sizeof buf)) return sd_errorf("os.cwd: %s", sd_strerror(errno));
  *out = sd_str_from_cstr(buf);
  return SD_ERR_NULL;
}

sd_err sd_os_run(const sd_list *argv, sd_int *code) {
  *code = SD_INT_NULL;
  if (argv->len == 0) sd_crash("os.run: argv is empty");
  sd_str const *xs = sd_list_data(argv);
  char **av = sd_alloc((size_t)(argv->len + 1) * sizeof(char *));
  for (int64_t i = 0; i < argv->len; i++) av[i] = (char *)xs[i]->data;
  av[argv->len] = NULL;
  sd_block_begin();
  fflush(stdout);
  fflush(stderr);
  pid_t pid;
  int r = posix_spawnp(&pid, av[0], NULL, NULL, av, environ);
  sd_block_end();
  if (r != 0) {
    sd_err e = sd_errorf("os.run: %s: %s", av[0], sd_strerror(r));
    free(av);
    return e;
  }
  int st = 0;
  int w = sd_wait_child(pid, &st); /* a stop point: the child is killed if the goroutine is stopped */
  if (w == SD_WAIT_STOP) {
    free(av);
    sd_stop();
  }
  if (w != 0) {
    sd_err e = sd_errorf("os.run: %s: %s", av[0], sd_strerror(w));
    free(av);
    return e;
  }
  /* glibc's posix_spawnp reports exec failure as exit status 127. */
  free(av);
  *code = SD_INT(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0));
  return SD_ERR_NULL;
}

/* ------------------------------------------------------------------- fs */

static sd_err fs_err(const char *fn, sd_str path, int e) { return sd_errorf("fs.%s: %s: %s", fn, path->data, sd_strerror(e)); }

static sd_err sd_fs_read_blocking_(sd_str path, sd_str *out) {
  *out = NULL;
  FILE *f = fopen(path->data, "rb");
  if (!f) return fs_err("read", path, errno);
  int64_t cap = 4096, n = 0;
  char *buf = sd_alloc((size_t)cap);
  for (;;) {
    if (n == cap) {
      cap *= 2;
      buf = sd_realloc(buf, (size_t)cap);
    }
    size_t r = fread(buf + n, 1, (size_t)(cap - n), f);
    n += (int64_t)r;
    if (r == 0) break;
  }
  int bad = ferror(f) ? (errno ? errno : EIO) : 0;
  fclose(f);
  if (bad) {
    free(buf);
    return fs_err("read", path, bad);
  }
  *out = sd_str_new(buf, n);
  free(buf);
  return SD_ERR_NULL;
}

static sd_err sd_fs_write_blocking_(sd_str path, sd_str data) {
  FILE *f = fopen(path->data, "wb");
  if (!f) return fs_err("write", path, errno);
  size_t w = fwrite(data->data, 1, (size_t)data->len, f);
  bool bad = fclose(f) != 0 || w != (size_t)data->len;
  if (bad) return fs_err("write", path, errno ? errno : EIO);
  return SD_ERR_NULL;
}

static bool sd_fs_exists_blocking_(sd_str path) {
  struct stat st;
  return stat(path->data, &st) == 0;
}

static bool sd_fs_is_dir_blocking_(sd_str path) {
  struct stat st;
  return stat(path->data, &st) == 0 && S_ISDIR(st.st_mode);
}

static sd_err sd_fs_list_blocking_(sd_str dir, sd_list **out) {
  *out = NULL;
  DIR *d = opendir(dir->data);
  if (!d) return fs_err("list", dir, errno);
  sd_list *l = sd_list_new(&sd_type_str, 8);
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    SD_LIST_PUSH(&l, sd_str, sd_str_from_cstr(de->d_name));
  }
  closedir(d);
  sd_list_sort(&l);
  *out = l;
  return SD_ERR_NULL;
}

static sd_err sd_fs_mkdir_blocking_(sd_str path) {
  char *cp = sd_alloc((size_t)path->len + 1);
  memcpy(cp, path->data, (size_t)path->len + 1);
  size_t n = strlen(cp);
  for (size_t i = 1; i <= n; i++) {
    if (i < n && cp[i] != '/') continue;
    char c = cp[i];
    cp[i] = 0;
    if (mkdir(cp, 0777) != 0 && errno != EEXIST) {
      int e = errno;
      sd_err err = fs_err("mkdir", path, e);
      free(cp);
      return err;
    }
    cp[i] = c;
  }
  struct stat st;
  sd_err err = SD_ERR_NULL;
  if (stat(cp, &st) != 0 || !S_ISDIR(st.st_mode)) err = fs_err("mkdir", path, ENOTDIR);
  free(cp);
  return err;
}

static sd_err sd_fs_remove_blocking_(sd_str path) {
  if (remove(path->data) != 0) return fs_err("remove", path, errno);
  return SD_ERR_NULL;
}

sd_err sd_fs_read(sd_str path, sd_str *out) {
  sd_block_begin();
  sd_err r = sd_fs_read_blocking_(path, out);
  sd_block_end();
  return r;
}

sd_err sd_fs_write(sd_str path, sd_str data) {
  sd_block_begin();
  sd_err r = sd_fs_write_blocking_(path, data);
  sd_block_end();
  return r;
}

bool sd_fs_exists(sd_str path) {
  sd_block_begin();
  bool r = sd_fs_exists_blocking_(path);
  sd_block_end();
  return r;
}

bool sd_fs_is_dir(sd_str path) {
  sd_block_begin();
  bool r = sd_fs_is_dir_blocking_(path);
  sd_block_end();
  return r;
}

sd_err sd_fs_list(sd_str dir, sd_list **out) {
  sd_block_begin();
  sd_err r = sd_fs_list_blocking_(dir, out);
  sd_block_end();
  return r;
}

sd_err sd_fs_mkdir(sd_str path) {
  sd_block_begin();
  sd_err r = sd_fs_mkdir_blocking_(path);
  sd_block_end();
  return r;
}

sd_err sd_fs_remove(sd_str path) {
  sd_block_begin();
  sd_err r = sd_fs_remove_blocking_(path);
  sd_block_end();
  return r;
}
