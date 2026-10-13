/* Core packages: os and fs (fmt is covered by test_exit.sh and the example). */
#include "check.h"

#include <unistd.h>

static void check_ok(sd_err e, int line) {
  check_count++;
  if (!sd_err_is_null(e)) {
    sd_str m = sd_err_message(e);
    check_failures++;
    fprintf(stderr, "test_pkg.c:%d: unexpected error: %s\n", line, sd_str_cstr(m));
    sd_str_release(m);
    sd_err_release(e);
  }
}
#define CHECK_OK(e) check_ok((e), __LINE__)

static sd_list *argv_of(const char *a, const char *b, const char *c) {
  sd_list *l = sd_list_new(&sd_type_str, 0);
  const char *xs[] = {a, b, c};
  for (int i = 0; i < 3 && xs[i]; i++) SD_LIST_PUSH(&l, sd_str, sd_str_from_cstr(xs[i]));
  return l;
}

static sd_err run_empty(void *argv) {
  sd_int code;
  return sd_os_run(argv, &code);
}

static void test_os(void) {
  sd_str path = SD_STR_LIT("PATH");
  sd_str v = sd_os_env(path);
  CHECK(v != NULL && v->len > 0);
  sd_str_release(v);
  CHECK(sd_os_env(SD_STR_LIT("SEED_SURELY_UNSET_VARIABLE_42")) == NULL);

  sd_int code;
  sd_list *av = argv_of("true", NULL, NULL);
  CHECK_OK(sd_os_run(av, &code));
  CHECK(sd_int_eq(code, SD_INT(0)));
  sd_list_release(av);
  av = argv_of("false", NULL, NULL);
  CHECK_OK(sd_os_run(av, &code));
  CHECK(sd_int_eq(code, SD_INT(1)));
  sd_list_release(av);
  av = argv_of("sh", "-c", "exit 7");
  CHECK_OK(sd_os_run(av, &code));
  CHECK(sd_int_eq(code, SD_INT(7)));
  sd_list_release(av);
  av = argv_of("seed-no-such-program-xyz", NULL, NULL);
  sd_err e = sd_os_run(av, &code);
#if defined(__APPLE__)
  CHECK(!sd_err_is_null(e) && sd_int_is_null(code)); /* could not be started */
#else
  /* glibc reports a failed exec either as an error or as exit status 127 */
  CHECK(!sd_err_is_null(e) || sd_int_eq(code, SD_INT(127)));
#endif
  sd_err_release(e);
  sd_list_release(av);
  av = sd_list_new(&sd_type_str, 0);
  e = sd_catch(run_empty, av); /* an empty argv is the caller's mistake: a crash */
  sd_str em = sd_err_message(e);
  CHECK(strcmp(sd_str_cstr(em), "crash: os.run: argv is empty") == 0);
  sd_str_release(em);
  sd_err_release(e);
  sd_list_release(av);
}

static void test_fs(void) {
  const char *tmp = getenv("TMPDIR");
  char base[512];
  snprintf(base, sizeof base, "%s/seed_rt_fs_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
  sd_str dir = sd_str_from_cstr(base);
  sd_str sub = sd_str_concat(dir, SD_STR_LIT("/a/b"));
  CHECK_OK(sd_fs_mkdir(sub));
  CHECK_OK(sd_fs_mkdir(sub)); /* exists: fine */
  CHECK(sd_fs_is_dir(sub) && sd_fs_exists(sub));
  sd_str f1 = sd_str_concat(sub, SD_STR_LIT("/z.txt")), f2 = sd_str_concat(sub, SD_STR_LIT("/y.txt"));
  sd_str data = sd_str_new("line1\nline2\0bin", 15);
  CHECK_OK(sd_fs_write(f1, data));
  CHECK_OK(sd_fs_write(f2, SD_STR_LIT("")));
  CHECK(sd_fs_exists(f1) && !sd_fs_is_dir(f1));
  sd_str back;
  CHECK_OK(sd_fs_read(f1, &back));
  CHECK(sd_str_eq(back, data));
  sd_str_release(back);
  sd_list *names;
  CHECK_OK(sd_fs_list(sub, &names));
  CHECK_SDSTR(sd_list_join(names, SD_STR_LIT(",")), "y.txt,z.txt");
  sd_list_release(names);
  /* errors */
  sd_str missing = sd_str_concat(dir, SD_STR_LIT("/missing"));
  sd_err e = sd_fs_read(missing, &back);
  CHECK(!sd_err_is_null(e) && back == NULL);
  sd_str m = sd_err_message(e);
  CHECK(strstr(sd_str_cstr(m), "fs.read: ") == sd_str_cstr(m));
  CHECK(strstr(sd_str_cstr(m), "/missing: no such file or directory") != NULL);
  sd_str_release(m);
  sd_err_release(e);
  e = sd_fs_list(missing, &names);
  CHECK(!sd_err_is_null(e) && names == NULL);
  sd_err_release(e);
  e = sd_fs_remove(sub); /* not empty */
  CHECK(!sd_err_is_null(e));
  sd_err_release(e);
  e = sd_fs_mkdir(f1); /* a file is in the way */
  CHECK(!sd_err_is_null(e));
  sd_err_release(e);
  /* clean up */
  CHECK_OK(sd_fs_remove(f1));
  CHECK_OK(sd_fs_remove(f2));
  CHECK_OK(sd_fs_remove(sub));
  sd_str a = sd_str_concat(dir, SD_STR_LIT("/a"));
  CHECK_OK(sd_fs_remove(a));
  CHECK_OK(sd_fs_remove(dir));
  CHECK(!sd_fs_exists(dir));
  sd_str *all[] = {&dir, &sub, &f1, &f2, &data, &missing, &a};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) sd_str_release(*all[i]);
}

int main(void) {
  int64_t live0 = sd_live_objects();
  test_os();
  test_fs();
  CHECK(sd_live_objects() == live0);
  return check_done("test_pkg");
}
