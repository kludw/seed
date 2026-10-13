/* Natives of package fs (runtime/README.md, "Native functions"). */
#include "seed.h"

sd_err sd_lib_fs_read(sd_str path, sd_str *out) { return sd_fs_read(path, out); }

sd_err sd_lib_fs_write(sd_str path, sd_str data) { return sd_fs_write(path, data); }

bool sd_lib_fs_exists(sd_str path) { return sd_fs_exists(path); }

bool sd_lib_fs_isDir(sd_str path) { return sd_fs_is_dir(path); }

sd_err sd_lib_fs_list(sd_str dir, sd_list **out) { return sd_fs_list(dir, out); }

sd_err sd_lib_fs_mkdir(sd_str path) { return sd_fs_mkdir(path); }

sd_err sd_lib_fs_remove(sd_str path) { return sd_fs_remove(path); }
