/* Natives of package fs (runtime/README.md, "Native functions"). Errors
 * follow docs/stdlib.md: "fs.<function>: <path>: <reason>", and
 * "fs.File.<method>: <path>: <reason>" for the methods of File. */
#include "seed.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static sd_err fs_errorf(const char *fn, const char *path, int e) { return sd_errorf("fs.%s: %s: %s", fn, path, sd_strerror(e)); }

/* Paths go to the OS as C strings, which end at the first NUL byte: a path
 * with a NUL inside would name a different file ("dir/\0x" is "dir/", so
 * removeAll would remove dir). Such a path is an error, as in Go; the
 * subject shows each NUL as \x00. */
static bool fs_has_nul(sd_str s) { return s->len > 0 && memchr(s->data, 0, (size_t)s->len) != NULL; }

static char *fs_show(sd_str s) {
  char *out = sd_alloc((size_t)s->len * 4 + 1), *q = out;
  for (int64_t i = 0; i < s->len; i++) {
    if (s->data[i] == 0) {
      memcpy(q, "\\x00", 4);
      q += 4;
    } else {
      *q++ = s->data[i];
    }
  }
  *q = 0;
  return out;
}

static sd_err fs_nul_error(const char *fn, sd_str path) {
  char *shown = fs_show(path);
  sd_err e = sd_errorf("fs.%s: %s: path contains a NUL byte", fn, shown);
  free(shown);
  return e;
}

#define FS_CHECK_PATH(fn, path) \
  do { \
    if (fs_has_nul(path)) return fs_nul_error(fn, path); \
  } while (0)

/* Opening a FIFO waits for its other end, and writing to one waits while
 * it is full, for ever if nobody reads: both are made in interruptible
 * block sections (seed.h), so that stopping the goroutine interrupts them
 * (EINTR). FS_STOPPED: the goroutine was stopped; the caller releases what
 * it holds and calls sd_stop(). On a regular file nothing is interrupted. */
#define FS_STOPPED (-1000)

/* Writes all n bytes to fd, retrying after short writes. Returns 0, the
 * errno, or FS_STOPPED. */
static int fs_write_all(int fd, const char *p, size_t n) {
  int e = 0;
  sd_block_begin_intr();
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) {
      if (sd_stopping()) {
        e = FS_STOPPED;
        break;
      }
      continue;
    }
    if (w < 0) {
      e = errno;
      break;
    }
    if (w == 0) {
      e = EIO;
      break;
    }
    p += w;
    n -= (size_t)w;
  }
  sd_block_end_intr();
  return e;
}

/* open(), in an interruptible block section (a FIFO waits for its other
 * end). Returns the descriptor, -1 with errno set, or FS_STOPPED. */
static int fs_open_retry(const char *path, int flags, mode_t mode) {
  int fd;
  sd_block_begin_intr();
  for (;;) {
    fd = open(path, flags | O_CLOEXEC, mode);
    if (fd >= 0 || errno != EINTR) break;
    if (sd_stopping()) {
      fd = FS_STOPPED;
      break;
    }
  }
  sd_block_end_intr();
  return fd;
}

/* ------------------------------------------------------- whole files */

sd_err sd_lib_fs_read(sd_str path, sd_str *out) {
  FS_CHECK_PATH("read", path);
  return sd_fs_read(path, out);
}

static sd_err fs_write_blocking(sd_str path, sd_str data) {
  FS_CHECK_PATH("write", path);
  int fd = fs_open_retry(path->data, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd == FS_STOPPED) sd_stop();
  if (fd < 0) return fs_errorf("write", path->data, errno);
  int e = fs_write_all(fd, data->data, (size_t)data->len);
  if (close(fd) != 0 && e == 0 && errno != EINTR) e = errno;
  if (e == FS_STOPPED) sd_stop();
  if (e) return fs_errorf("write", path->data, e);
  return SD_ERR_NULL;
}

/* One append at a time in this process, so that two goroutines' appends
 * never interleave even when a write() is cut short; O_APPEND keeps whole
 * writes from different processes apart on a local file system. */
static pthread_mutex_t fs_append_mu = PTHREAD_MUTEX_INITIALIZER;

/* Takes fs_append_mu. Another goroutine's append may hold it for long (to
 * a full FIFO), so a stopped goroutine gives up after waiting 10 ms
 * (false). A short wait, for an append to a regular file, does not stop
 * it: its defers may append to a log. */
static bool fs_append_lock(void) {
  long waited = 0;
  for (long ns = 1000;; ns = ns < 1000000 ? ns * 2 : ns) {
    if (pthread_mutex_trylock(&fs_append_mu) == 0) return true;
    if (waited >= 10000000 && sd_stopping()) return false;
    waited += ns;
    if (ns < 16000) {
      sched_yield();
      continue;
    }
    struct timespec ts = {0, ns};
    nanosleep(&ts, NULL);
  }
}

static sd_err fs_append_blocking(sd_str path, sd_str data) {
  FS_CHECK_PATH("append", path);
  int fd = fs_open_retry(path->data, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd == FS_STOPPED) sd_stop();
  if (fd < 0) return fs_errorf("append", path->data, errno);
  if (!fs_append_lock()) {
    close(fd);
    sd_stop();
  }
  int e = fs_write_all(fd, data->data, (size_t)data->len);
  pthread_mutex_unlock(&fs_append_mu);
  if (close(fd) != 0 && e == 0 && errno != EINTR) e = errno;
  if (e == FS_STOPPED) sd_stop();
  if (e) return fs_errorf("append", path->data, e);
  return SD_ERR_NULL;
}

bool sd_lib_fs_exists(sd_str path) { return !fs_has_nul(path) && sd_fs_exists(path); }

bool sd_lib_fs_isDir(sd_str path) { return !fs_has_nul(path) && sd_fs_is_dir(path); }

static sd_err fs_stat_blocking(sd_str path, sd_S_fs_Info *out) {
  FS_CHECK_PATH("stat", path);
  struct stat st;
  if (stat(path->data, &st) != 0) return fs_errorf("stat", path->data, errno);
#if defined(__APPLE__)
  int64_t sec = (int64_t)st.st_mtimespec.tv_sec, nsec = (int64_t)st.st_mtimespec.tv_nsec;
#else
  int64_t sec = (int64_t)st.st_mtim.tv_sec, nsec = (int64_t)st.st_mtim.tv_nsec;
#endif
  out->f_size = sd_int_from_i64((int64_t)st.st_size);
  out->f_isDir = S_ISDIR(st.st_mode);
  out->f_modified = sd_int_from_i64(sec * 1000000000 + nsec);
  return SD_ERR_NULL;
}

/* ------------------------------------------------------- directories */

sd_err sd_lib_fs_list(sd_str dir, sd_list **out) {
  FS_CHECK_PATH("list", dir);
  return sd_fs_list(dir, out);
}

/* Like the runtime's sd_fs_mkdir (error messages included), with the mode
 * docs/stdlib.md gives. */
static sd_err fs_mkdir_blocking(sd_str path) {
  FS_CHECK_PATH("mkdir", path);
  char *cp = sd_alloc((size_t)path->len + 1);
  memcpy(cp, path->data, (size_t)path->len + 1);
  size_t n = strlen(cp);
  for (size_t i = 1; i <= n; i++) {
    if (i < n && cp[i] != '/') continue;
    char c = cp[i];
    cp[i] = 0;
    if (mkdir(cp, 0755) != 0 && errno != EEXIST) {
      sd_err err = fs_errorf("mkdir", path->data, errno);
      free(cp);
      return err;
    }
    cp[i] = c;
  }
  struct stat st;
  sd_err err = SD_ERR_NULL;
  if (stat(cp, &st) != 0 || !S_ISDIR(st.st_mode)) err = fs_errorf("mkdir", path->data, ENOTDIR);
  free(cp);
  return err;
}

static sd_err fs_rename_blocking(sd_str from, sd_str to) {
  if (fs_has_nul(from) || fs_has_nul(to)) {
    char *a = fs_show(from), *b = fs_show(to);
    sd_err e = sd_errorf("fs.rename: %s -> %s: path contains a NUL byte", a, b);
    free(a);
    free(b);
    return e;
  }
  if (rename(from->data, to->data) != 0) return sd_errorf("fs.rename: %s -> %s: %s", from->data, to->data, sd_strerror(errno));
  return SD_ERR_NULL;
}

static sd_err fs_remove_blocking(sd_str path) {
  FS_CHECK_PATH("remove", path);
  return sd_fs_remove(path);
}

static char *fs_join(const char *dir, const char *name) {
  size_t a = strlen(dir), b = strlen(name);
  char *p = sd_alloc(a + b + 2);
  memcpy(p, dir, a);
  p[a] = '/';
  memcpy(p + a + 1, name, b + 1);
  return p;
}

/* Removes path and what is inside it, without following symbolic links.
 * The names in a directory are read first and removed after closing it, so
 * that removing does not disturb readdir. */
static sd_err fs_remove_tree(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) return SD_ERR_NULL;
    return fs_errorf("removeAll", path, errno);
  }
  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0 && errno != ENOENT) return fs_errorf("removeAll", path, errno);
    return SD_ERR_NULL;
  }
  DIR *d = opendir(path);
  if (!d) return fs_errorf("removeAll", path, errno);
  size_t n = 0, cap = 16;
  char **names = sd_alloc(cap * sizeof(char *));
  struct dirent *de;
  while ((de = readdir(d))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    if (n == cap) {
      cap *= 2;
      names = sd_realloc(names, cap * sizeof(char *));
    }
    names[n++] = fs_join(path, de->d_name);
  }
  closedir(d);
  sd_err err = SD_ERR_NULL;
  for (size_t i = 0; i < n; i++) {
    if (sd_err_is_null(err)) err = fs_remove_tree(names[i]);
    free(names[i]);
  }
  free(names);
  if (!sd_err_is_null(err)) return err;
  if (rmdir(path) != 0 && errno != ENOENT) return fs_errorf("removeAll", path, errno);
  return SD_ERR_NULL;
}

static sd_err fs_removeAll_blocking(sd_str path) {
  FS_CHECK_PATH("removeAll", path);
  if (path->len == 0) return SD_ERR_NULL;
  return fs_remove_tree(path->data);
}

static sd_err fs_makeTemp_blocking(sd_str prefix, sd_str *out) {
  FS_CHECK_PATH("makeTemp", prefix);
  const char *dir = getenv("TMPDIR");
  if (!dir || !*dir) dir = "/tmp";
  size_t dl = strlen(dir);
  while (dl > 1 && dir[dl - 1] == '/') dl--;
  size_t pl = (size_t)prefix->len;
  char *tmpl = sd_alloc(dl + 1 + pl + 7);
  memcpy(tmpl, dir, dl);
  tmpl[dl] = '/';
  memcpy(tmpl + dl + 1, prefix->data, pl);
  memcpy(tmpl + dl + 1 + pl, "XXXXXX", 7);
  if (!mkdtemp(tmpl)) {
    tmpl[dl + 1 + pl] = 0;
    sd_err err = fs_errorf("makeTemp", tmpl, errno);
    free(tmpl);
    return err;
  }
  *out = sd_str_from_cstr(tmpl);
  free(tmpl);
  return SD_ERR_NULL;
}

/* ------------------------------------------------------- File handles */

/* A File is an entry of the runtime's handle table (runtime/README.md,
 * section 13; kind SD_HANDLE_FILE), shared with io and net: the File holds
 * its id, whether it was opened for writing, and the path (for messages,
 * also after close). The table makes every operation atomic (one at a
 * time per handle), and its reads poll before they read, so a read is a
 * stop point (a FIFO can block for ever). Ids are never reused, so a stale
 * copy of a closed File finds nothing and says "file is closed". */

static sd_err fs_open_file(const char *fn, sd_str path, int flags, bool writing, sd_S_fs_File *out) {
  FS_CHECK_PATH(fn, path);
  int fd = fs_open_retry(path->data, flags, 0644);
  if (fd == FS_STOPPED) sd_stop();
  if (fd < 0) return fs_errorf(fn, path->data, errno);
  if (!writing) {
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
      close(fd);
      return fs_errorf(fn, path->data, EISDIR);
    }
  }
  out->f_id = sd_int_from_i64(sd_handle_new(SD_HANDLE_FILE, fd, path->data));
  out->f_writing = writing;
  out->f_path = sd_str_retain(path);
  return SD_ERR_NULL;
}

sd_err sd_lib_fs_open(sd_str path, sd_S_fs_File *out) { return fs_open_file("open", path, O_RDONLY, false, out); }

sd_err sd_lib_fs_create(sd_str path, sd_S_fs_File *out) {
  return fs_open_file("create", path, O_WRONLY | O_CREAT | O_TRUNC, true, out);
}

sd_err sd_lib_fs_openAppend(sd_str path, sd_S_fs_File *out) {
  return fs_open_file("openAppend", path, O_WRONLY | O_CREAT | O_APPEND, true, out);
}

/* The handle of f, acquired (released with sd_handle_release) and open for
 * reading (want_write false) or writing; otherwise NULL with *err set. */
static sd_handle *fs_handle_use(sd_S_fs_File f, const char *method, bool want_write, sd_err *err) {
  const char *why = NULL;
  if (want_write && !f.f_writing) why = "file is open for reading only";
  else if (!want_write && f.f_writing) why = "file is open for writing only";
  sd_handle *h = NULL;
  int64_t id;
  if (!why && sd_int_to_i64(f.f_id, &id)) h = sd_handle_acquire(id, SD_HANDLE_FILE);
  if (h) return h;
  *err = sd_errorf("fs.File.%s: %s: %s", method, f.f_path->data, why ? why : "file is closed");
  return NULL;
}

/* The error for a handle I/O result other than SD_IO_OK and SD_IO_EOF. */
static sd_err fs_io_error(const char *method, sd_S_fs_File f, int r, int e) {
  if (r == SD_IO_CLOSED) return sd_errorf("fs.File.%s: %s: file is closed", method, f.f_path->data);
  if (r == SD_IO_TIMEOUT) return sd_errorf("fs.File.%s: %s: timeout", method, f.f_path->data);
  return sd_errorf("fs.File.%s: %s: %s", method, f.f_path->data, sd_strerror(e));
}

sd_err sd_lib_fs_File_readLine(sd_S_fs_File f, sd_str *out) {
  sd_err err = SD_ERR_NULL;
  sd_handle *h = fs_handle_use(f, "readLine", false, &err);
  if (!h) return err;
  int e = 0;
  int r = sd_handle_read_line(h, out, &e);
  if (r != SD_IO_OK && r != SD_IO_EOF) err = fs_io_error("readLine", f, r, e);
  sd_handle_release(h);
  return err;
}

sd_err sd_lib_fs_File_read(sd_S_fs_File f, sd_int max, sd_str *out) {
  int64_t m;
  if (!sd_int_to_i64(max, &m)) m = sd_int_lt(max, SD_INT(0)) ? -1 : INT64_MAX;
  if (m < 1) {
    sd_crash_int("fs.File.read: max (%s) must be at least 1", max);
  }
  sd_err err = SD_ERR_NULL;
  sd_handle *h = fs_handle_use(f, "read", false, &err);
  if (!h) return err;
  int e = 0;
  int r = sd_handle_read(h, m, out, &e);
  if (r != SD_IO_OK && r != SD_IO_EOF) err = fs_io_error("read", f, r, e);
  sd_handle_release(h);
  return err;
}

sd_err sd_lib_fs_File_write(sd_S_fs_File f, sd_str data) {
  sd_err err = SD_ERR_NULL;
  sd_handle *h = fs_handle_use(f, "write", true, &err);
  if (!h) return err;
  int e = 0;
  int r = sd_handle_write(h, data->data, data->len, &e);
  if (r != SD_IO_OK) err = fs_io_error("write", f, r, e);
  sd_handle_release(h);
  return err;
}

void sd_lib_fs_File_close(sd_S_fs_File f) {
  int64_t id;
  if (sd_int_to_i64(f.f_id, &id)) (void)sd_handle_close(id);
}

/* The natives above that use the file system directly run in block
 * sections (seed.h): a slow disk or a FIFO must not hold up the other
 * goroutines. */

sd_err sd_lib_fs_write(sd_str path, sd_str data) {
  sd_block_begin();
  sd_err r = fs_write_blocking(path, data);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_append(sd_str path, sd_str data) {
  sd_block_begin();
  sd_err r = fs_append_blocking(path, data);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_stat(sd_str path, sd_S_fs_Info *out) {
  sd_block_begin();
  sd_err r = fs_stat_blocking(path, out);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_mkdir(sd_str path) {
  sd_block_begin();
  sd_err r = fs_mkdir_blocking(path);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_rename(sd_str from, sd_str to) {
  sd_block_begin();
  sd_err r = fs_rename_blocking(from, to);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_remove(sd_str path) {
  sd_block_begin();
  sd_err r = fs_remove_blocking(path);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_removeAll(sd_str path) {
  sd_block_begin();
  sd_err r = fs_removeAll_blocking(path);
  sd_block_end();
  return r;
}

sd_err sd_lib_fs_makeTemp(sd_str prefix, sd_str *out) {
  sd_block_begin();
  sd_err r = fs_makeTemp_blocking(prefix, out);
  sd_block_end();
  return r;
}
