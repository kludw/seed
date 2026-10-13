/* Core packages: fmt, os, fs. (errors is in seed_core.c.) */
#include "seed_internal.h"

#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* ------------------------------------------------------------------ fmt */

void sd_fmt_print(sd_str s) {
  flockfile(stdout);
  fwrite(s->data, 1, (size_t)s->len, stdout);
  funlockfile(stdout);
}

void sd_fmt_println(sd_str s) {
  flockfile(stdout);
  fwrite(s->data, 1, (size_t)s->len, stdout);
  putc_unlocked('\n', stdout);
  funlockfile(stdout);
}

void sd_fmt_eprint(sd_str s) {
  fflush(stdout);
  flockfile(stderr);
  fwrite(s->data, 1, (size_t)s->len, stderr);
  funlockfile(stderr);
}

void sd_fmt_eprintln(sd_str s) {
  fflush(stdout);
  flockfile(stderr);
  fwrite(s->data, 1, (size_t)s->len, stderr);
  putc_unlocked('\n', stderr);
  funlockfile(stderr);
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

sd_err sd_os_run(const sd_list *argv, sd_int *code) {
  *code = SD_INT_NULL;
  if (argv->len == 0) return sd_errorf("os.run: empty argument list");
  sd_str const *xs = sd_list_data(argv);
  char **av = sd_alloc((size_t)(argv->len + 1) * sizeof(char *));
  for (int64_t i = 0; i < argv->len; i++) av[i] = (char *)xs[i]->data;
  av[argv->len] = NULL;
  fflush(stdout);
  fflush(stderr);
  pid_t pid;
  int r = posix_spawnp(&pid, av[0], NULL, NULL, av, environ);
  if (r != 0) {
    sd_err e = sd_errorf("%s: %s", av[0], strerror(r));
    free(av);
    return e;
  }
  int st;
  while (waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) {
      sd_err e = sd_errorf("%s: %s", av[0], strerror(errno));
      free(av);
      return e;
    }
  }
  /* glibc's posix_spawnp reports exec failure as exit status 127. */
  free(av);
  *code = SD_INT(WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0));
  return SD_ERR_NULL;
}

/* ------------------------------------------------------------------- fs */

static sd_err fs_err(sd_str path, int e) { return sd_errorf("%s: %s", path->data, strerror(e)); }

sd_err sd_fs_read(sd_str path, sd_str *out) {
  *out = NULL;
  FILE *f = fopen(path->data, "rb");
  if (!f) return fs_err(path, errno);
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
  int bad = ferror(f);
  fclose(f);
  if (bad) {
    free(buf);
    return sd_errorf("%s: read error", path->data);
  }
  *out = sd_str_new(buf, n);
  free(buf);
  return SD_ERR_NULL;
}

sd_err sd_fs_write(sd_str path, sd_str data) {
  FILE *f = fopen(path->data, "wb");
  if (!f) return fs_err(path, errno);
  size_t w = fwrite(data->data, 1, (size_t)data->len, f);
  bool bad = fclose(f) != 0 || w != (size_t)data->len;
  if (bad) return sd_errorf("%s: write error", path->data);
  return SD_ERR_NULL;
}

bool sd_fs_exists(sd_str path) {
  struct stat st;
  return stat(path->data, &st) == 0;
}

bool sd_fs_is_dir(sd_str path) {
  struct stat st;
  return stat(path->data, &st) == 0 && S_ISDIR(st.st_mode);
}

sd_err sd_fs_list(sd_str dir, sd_list **out) {
  *out = NULL;
  DIR *d = opendir(dir->data);
  if (!d) return fs_err(dir, errno);
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

sd_err sd_fs_mkdir(sd_str path) {
  char *cp = sd_alloc((size_t)path->len + 1);
  memcpy(cp, path->data, (size_t)path->len + 1);
  size_t n = strlen(cp);
  for (size_t i = 1; i <= n; i++) {
    if (i < n && cp[i] != '/') continue;
    char c = cp[i];
    cp[i] = 0;
    if (mkdir(cp, 0777) != 0 && errno != EEXIST) {
      int e = errno;
      sd_err err = sd_errorf("%s: %s", cp, strerror(e));
      free(cp);
      return err;
    }
    cp[i] = c;
  }
  struct stat st;
  sd_err err = SD_ERR_NULL;
  if (stat(cp, &st) != 0 || !S_ISDIR(st.st_mode)) err = sd_errorf("%s: not a directory", cp);
  free(cp);
  return err;
}

sd_err sd_fs_remove(sd_str path) {
  if (remove(path->data) != 0) return fs_err(path, errno);
  return SD_ERR_NULL;
}
