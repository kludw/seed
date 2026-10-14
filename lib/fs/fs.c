/* Natives of package fs (runtime/README.md, "Native functions"). Errors
 * follow docs/stdlib.md: "fs.<function>: <path>: <reason>", and
 * "fs.File.<method>: <path>: <reason>" for the methods of File. */
#include "seed.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static sd_err fs_errorf(const char *fn, const char *path, int e) { return sd_errorf("fs.%s: %s: %s", fn, path, sd_strerror(e)); }

/* Writes all n bytes to fd, retrying after signals and short writes.
 * Returns 0 or the errno. */
static int fs_write_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0) return errno;
    if (w == 0) return EIO;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int fs_open_retry(const char *path, int flags, mode_t mode) {
  int fd;
  do fd = open(path, flags | O_CLOEXEC, mode);
  while (fd < 0 && errno == EINTR);
  return fd;
}

/* ------------------------------------------------------- whole files */

sd_err sd_lib_fs_read(sd_str path, sd_str *out) { return sd_fs_read(path, out); }

sd_err sd_lib_fs_write(sd_str path, sd_str data) {
  int fd = fs_open_retry(path->data, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return fs_errorf("write", path->data, errno);
  int e = fs_write_all(fd, data->data, (size_t)data->len);
  if (close(fd) != 0 && e == 0 && errno != EINTR) e = errno;
  if (e) return fs_errorf("write", path->data, e);
  return SD_ERR_NULL;
}

/* One append at a time in this process, so that two goroutines' appends
 * never interleave even when a write() is cut short; O_APPEND keeps whole
 * writes from different processes apart on a local file system. */
static pthread_mutex_t fs_append_mu = PTHREAD_MUTEX_INITIALIZER;

sd_err sd_lib_fs_append(sd_str path, sd_str data) {
  int fd = fs_open_retry(path->data, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return fs_errorf("append", path->data, errno);
  pthread_mutex_lock(&fs_append_mu);
  int e = fs_write_all(fd, data->data, (size_t)data->len);
  pthread_mutex_unlock(&fs_append_mu);
  if (close(fd) != 0 && e == 0 && errno != EINTR) e = errno;
  if (e) return fs_errorf("append", path->data, e);
  return SD_ERR_NULL;
}

bool sd_lib_fs_exists(sd_str path) { return sd_fs_exists(path); }

bool sd_lib_fs_isDir(sd_str path) { return sd_fs_is_dir(path); }

sd_err sd_lib_fs_stat(sd_str path, sd_S_fs_Info *out) {
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

sd_err sd_lib_fs_list(sd_str dir, sd_list **out) { return sd_fs_list(dir, out); }

/* Like the runtime's sd_fs_mkdir (error messages included), with the mode
 * docs/stdlib.md gives. */
sd_err sd_lib_fs_mkdir(sd_str path) {
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

sd_err sd_lib_fs_rename(sd_str from, sd_str to) {
  if (rename(from->data, to->data) != 0) return sd_errorf("fs.rename: %s -> %s: %s", from->data, to->data, sd_strerror(errno));
  return SD_ERR_NULL;
}

sd_err sd_lib_fs_remove(sd_str path) { return sd_fs_remove(path); }

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

sd_err sd_lib_fs_removeAll(sd_str path) {
  if (path->len == 0) return SD_ERR_NULL;
  return fs_remove_tree(path->data);
}

sd_err sd_lib_fs_makeTemp(sd_str prefix, sd_str *out) {
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
  int fd = fs_open_retry(path->data, flags, 0644);
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
    sd_str s = sd_int_to_str(max);
    sd_crash("fs.File.read: max (%s) must be at least 1", s->data);
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
