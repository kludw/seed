/* Natives of package io (runtime/README.md, "Native functions"). Stdin is a
 * handle of the runtime's table (kind SD_HANDLE_STDIN), so that its reads
 * are stop points, serialised between goroutines and buffered. */
#include "seed.h"

#include <pthread.h>
#include <stdio.h>

static int64_t io_stdin_id;
static pthread_once_t io_stdin_once = PTHREAD_ONCE_INIT;

static void io_stdin_init(void) { io_stdin_id = sd_handle_new(SD_HANDLE_STDIN, 0, ""); }

static sd_handle *io_stdin(void) {
  pthread_once(&io_stdin_once, io_stdin_init);
  sd_block_begin(); /* stdout may be a full pipe */
  fflush(stdout);
  sd_block_end();
  return sd_handle_acquire(io_stdin_id, SD_HANDLE_STDIN);
}

sd_err sd_lib_io_readLine(sd_str *out) {
  sd_handle *h = io_stdin();
  int err = 0;
  int r = sd_handle_read_line(h, out, &err);
  sd_err e = SD_ERR_NULL;
  if (r != SD_IO_OK && r != SD_IO_EOF) e = sd_handle_error("io.readLine", h, SD_HANDLE_STDIN, r, err);
  sd_handle_release(h);
  return e;
}

sd_err sd_lib_io_read(sd_int max, sd_str *out) {
  int64_t m;
  if (!sd_int_to_i64(max, &m)) m = sd_int_lt(max, SD_INT(0)) ? -1 : INT64_MAX;
  if (m < 1) {
    sd_crash_int("io.read: max (%s) must be at least 1", max);
  }
  sd_handle *h = io_stdin();
  int err = 0;
  int r = sd_handle_read(h, m, out, &err);
  sd_err e = SD_ERR_NULL;
  if (r != SD_IO_OK && r != SD_IO_EOF) e = sd_handle_error("io.read", h, SD_HANDLE_STDIN, r, err);
  sd_handle_release(h);
  return e;
}

sd_err sd_lib_io_readAll(sd_str *out) {
  sd_handle *h = io_stdin();
  int err = 0;
  int r = sd_handle_read_all(h, out, &err);
  sd_err e = SD_ERR_NULL;
  if (r != SD_IO_OK) e = sd_handle_error("io.readAll", h, SD_HANDLE_STDIN, r, err);
  sd_handle_release(h);
  return e;
}
