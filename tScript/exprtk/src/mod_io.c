/**
 * @file exprtk_mod_io.c
 * @brief IO module: file operations, path utilities, date/time functions.
 *
 * All functions below are exposed in the "io" namespace (global registry).
 * They are always available in any exprtk_env_t, even outside TurboScript.
 */
#include "datetime_parser.h"
#include "exprtk_module.h"
#include "turbo_fs.h"


/* ── Helper: null-terminate a tstr_v into the arena ────────────── */
static char *arena_path(turbo_pool_t *arena, tstr_v sv) {
  char *p = turbo_pool_alloc(arena, sv.len + 1);
  if (p) {
    memcpy(p, sv.data, sv.len);
    p[sv.len] = '\0';
  }
  return p;
}

/* ═══════════════════════════════════════════════════════════════════
 * File Read / Write
 * ═══════════════════════════════════════════════════════════════════ */

/** read_file(path) → string contents (or 0 on failure) */
static exprtk_value_t fn_read_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_buf_t buf = {0};
      if (turbo_fs_read_file(path, &buf) == 0) {
        char *data = turbo_pool_alloc(arena, buf.len);
        if (data) {
          memcpy(data, buf.base, buf.len);
          turbo_fs_buf_free(&buf);
          return exprtk_val_str(tstr_v_from_buf(data, buf.len));
        }
        turbo_fs_buf_free(&buf);
      }
    }
  }
  return exprtk_val_num(0);
}

/** write_file(path, content) → 0 on success */
static exprtk_value_t fn_write_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                    turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_buf_t buf;
      buf.base = (char *)args[1].data.string.data;
      buf.len = args[1].data.string.len;
      return exprtk_val_num((double)turbo_fs_write_file(path, &buf));
    }
  }
  return exprtk_val_num(0);
}

/** append_file(path, content) → 0 on success */
static exprtk_value_t fn_append_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_file_t fd = turbo_fs_open(
          path, TURBO_FS_O_WRONLY | TURBO_FS_O_CREAT | TURBO_FS_O_APPEND, TURBO_FS_DEFAULT_MODE);
      if (fd == TURBO_INVALID_FILE)
        return exprtk_val_num(-1);
      int res = turbo_fs_write(fd, args[1].data.string.data, args[1].data.string.len);
      turbo_fs_close(fd);
      return exprtk_val_num(res >= 0 ? 0.0 : (double)res);
    }
  }
  return exprtk_val_num(-1);
}

/* ═══════════════════════════════════════════════════════════════════
 * File Queries
 * ═══════════════════════════════════════════════════════════════════ */

/** file_exists(path) → 1.0 or 0.0 */
static exprtk_value_t fn_file_exists(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_stat_t st;
      return exprtk_val_num(turbo_fs_stat(path, &st) == 0 ? 1.0 : 0.0);
    }
  }
  return exprtk_val_num(0);
}

/** file_size(path) → size in bytes (or -1 on failure) */
static exprtk_value_t fn_file_size(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0)
        return exprtk_val_num((double)st.size);
    }
  }
  return exprtk_val_num(-1);
}

/** file_stat(path) → vector [size, mtime, is_file, is_dir] (or 0 on failure) */
static exprtk_value_t fn_file_stat(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0) {
        double *out = TURBO_POOL_ALLOC_ARRAY(arena, double, 4);
        if (out) {
          out[0] = (double)st.size;
          out[1] = (double)(st.mtime / 1000000ULL); /* µs → seconds */
          out[2] = st.is_file ? 1.0 : 0.0;
          out[3] = st.is_directory ? 1.0 : 0.0;
          return exprtk_val_vec(out, 4);
        }
      }
    }
  }
  return exprtk_val_num(0);
}

/** is_file(path) → 1.0 if regular file, else 0.0 */
static exprtk_value_t fn_is_file(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0)
        return exprtk_val_num(st.is_file ? 1.0 : 0.0);
    }
  }
  return exprtk_val_num(0);
}

/** is_dir(path) → 1.0 if directory, else 0.0 */
static exprtk_value_t fn_is_dir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      turbo_fs_stat_t st;
      if (turbo_fs_stat(path, &st) == 0)
        return exprtk_val_num(st.is_directory ? 1.0 : 0.0);
    }
  }
  return exprtk_val_num(0);
}

/* ═══════════════════════════════════════════════════════════════════
 * File Manipulation
 * ═══════════════════════════════════════════════════════════════════ */

/** file_remove(path) → 0 on success */
static exprtk_value_t fn_file_remove(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path)
      return exprtk_val_num((double)turbo_fs_unlink(path));
  }
  return exprtk_val_num(-1);
}

/** file_rename(old_path, new_path) → 0 on success */
static exprtk_value_t fn_file_rename(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *old_path = arena_path(arena, args[0].data.string);
    char *new_path = arena_path(arena, args[1].data.string);
    if (old_path && new_path)
      return exprtk_val_num((double)turbo_fs_rename(old_path, new_path));
  }
  return exprtk_val_num(-1);
}

/* ═══════════════════════════════════════════════════════════════════
 * Directory Operations
 * ═══════════════════════════════════════════════════════════════════ */

/** mkdir(path) → 0 on success */
static exprtk_value_t fn_mkdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  if (argc >= 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      int mode = (argc >= 2 && args[1].type == EXPRTK_VAL_NUMBER) ? (int)args[1].data.number : 0755;
      return exprtk_val_num((double)turbo_fs_mkdir(path, mode));
    }
  }
  return exprtk_val_num(-1);
}

/** rmdir(path) → 0 on success */
static exprtk_value_t fn_rmdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                               turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path)
      return exprtk_val_num((double)turbo_fs_rmdir(path));
  }
  return exprtk_val_num(-1);
}

/** tmpdir() → temporary directory path string */
static exprtk_value_t fn_tmpdir(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                turbo_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  char buf[TURBO_FS_MAX_PATH];
  if (turbo_fs_get_tmpdir(buf, sizeof(buf)) == 0) {
    size_t len = strlen(buf);
    char *out = turbo_pool_alloc(arena, len + 1);
    if (out) {
      memcpy(out, buf, len + 1);
      return exprtk_val_str(tstr_v_from_buf(out, len));
    }
  }
  return exprtk_val_num(0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Path Utilities
 * ═══════════════════════════════════════════════════════════════════ */

/** path_join(base, relative) → joined path string */
static exprtk_value_t fn_path_join(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == EXPRTK_VAL_STRING && args[1].type == EXPRTK_VAL_STRING) {
    char *base = arena_path(arena, args[0].data.string);
    char *rel = arena_path(arena, args[1].data.string);
    if (base && rel) {
      char buf[TURBO_FS_MAX_PATH * 2];
      if (turbo_fs_path_join(buf, sizeof(buf), base, rel) == 0) {
        size_t len = strlen(buf);
        char *out = turbo_pool_alloc(arena, len + 1);
        if (out) {
          memcpy(out, buf, len + 1);
          return exprtk_val_str(tstr_v_from_buf(out, len));
        }
      }
    }
  }
  return exprtk_val_num(0);
}

/** path_dirname(path) → directory component string */
static exprtk_value_t fn_path_dirname(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      char buf[TURBO_FS_MAX_PATH];
      if (turbo_fs_path_dirname(path, buf, sizeof(buf)) == 0) {
        size_t len = strlen(buf);
        char *out = turbo_pool_alloc(arena, len + 1);
        if (out) {
          memcpy(out, buf, len + 1);
          return exprtk_val_str(tstr_v_from_buf(out, len));
        }
      }
    }
  }
  return exprtk_val_num(0);
}

/** path_basename(path) → filename component string */
static exprtk_value_t fn_path_basename(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                       turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path) {
      char buf[TURBO_FS_MAX_PATH];
      if (turbo_fs_path_basename(path, buf, sizeof(buf)) == 0) {
        size_t len = strlen(buf);
        char *out = turbo_pool_alloc(arena, len + 1);
        if (out) {
          memcpy(out, buf, len + 1);
          return exprtk_val_str(tstr_v_from_buf(out, len));
        }
      }
    }
  }
  return exprtk_val_num(0);
}

/** path_is_absolute(path) → 1.0 or 0.0 */
static exprtk_value_t fn_path_is_absolute(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                          turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    char *path = arena_path(arena, args[0].data.string);
    if (path)
      return exprtk_val_num(turbo_fs_path_is_absolute(path) ? 1.0 : 0.0);
  }
  return exprtk_val_num(0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Date / Time
 * ═══════════════════════════════════════════════════════════════════ */

/** now() → Unix timestamp (seconds) */
static exprtk_value_t fn_now(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)time(NULL));
}

/** date(str) → Unix timestamp (seconds) */
static exprtk_value_t fn_date(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                              turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == EXPRTK_VAL_STRING) {
    datetime_t dt;
    char *ds = arena_path(arena, args[0].data.string);
    if (ds) {
      if (datetime_parse(ds, args[0].data.string.len, &dt) == 0)
        return exprtk_val_num((double)datetime_to_time(&dt));
    }
  }
  return exprtk_val_num(0);
}

/** format_date(timestamp [, fmt]) → formatted date string */
static exprtk_value_t fn_format_date(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                     turbo_pool_t *arena) {
  (void)env;
  if (argc == 1 || argc == 2) {
    time_t t = (time_t)args[0].data.number;
    char buf[128];
    if (argc == 2 && args[1].type == EXPRTK_VAL_STRING) {
      char *fmt = arena_path(arena, args[1].data.string);
      if (fmt) {
        struct tm *tm_info = localtime(&t);
        if (tm_info && strftime(buf, sizeof(buf), fmt, tm_info) > 0) {
          size_t len = strlen(buf);
          char *res_buf = turbo_pool_alloc(arena, len);
          if (res_buf) {
            memcpy(res_buf, buf, len);
            return exprtk_val_str(tstr_v_from_buf(res_buf, len));
          }
        }
      }
    } else {
      if (datetime_format_rfc822(t, buf, sizeof(buf)) > 0) {
        size_t len = strlen(buf);
        char *res_buf = turbo_pool_alloc(arena, len);
        if (res_buf) {
          memcpy(res_buf, buf, len);
          return exprtk_val_str(tstr_v_from_buf(res_buf, len));
        }
      }
    }
  }
  return exprtk_val_num(0);
}

/* ═══════════════════════════════════════════════════════════════════
 * Platform Info
 * ═══════════════════════════════════════════════════════════════════ */

/** os_name() → "windows", "linux", "macos", or "unknown" */
static exprtk_value_t fn_os_name(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                 turbo_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
#ifdef _WIN32
  return exprtk_val_str(tstr_v_from_cstr("windows"));
#elif defined(__APPLE__)
  return exprtk_val_str(tstr_v_from_cstr("macos"));
#elif defined(__linux__)
  return exprtk_val_str(tstr_v_from_cstr("linux"));
#else
  return exprtk_val_str(tstr_v_from_cstr("unknown"));
#endif
}

/** pid() → current process ID */
static exprtk_value_t fn_pid(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                             turbo_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)turbo_getpid());
}

/** uptime_ms() → milliseconds since process start */
static exprtk_value_t fn_uptime_ms(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                   turbo_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)turbo_uptime_ms());
}

/** monotonic_ms() → monotonic clock in milliseconds (never goes backward) */
static exprtk_value_t fn_monotonic_ms(size_t argc, exprtk_value_t *args, exprtk_env_t *env,
                                      turbo_pool_t *arena) {
  (void)argc;
  (void)args;
  (void)env;
  (void)arena;
  return exprtk_val_num((double)turbo_monotonic_ms());
}

/* ═══════════════════════════════════════════════════════════════════
 * Module Descriptor  (entries MUST be sorted alphabetically)
 * ═══════════════════════════════════════════════════════════════════ */

static const exprtk_func_entry_t io_entries[] = {
    {"append_file", fn_append_file},
    {"date", fn_date},
    {"file_exists", fn_file_exists},
    {"file_remove", fn_file_remove},
    {"file_rename", fn_file_rename},
    {"file_size", fn_file_size},
    {"file_stat", fn_file_stat},
    {"format_date", fn_format_date},
    {"is_dir", fn_is_dir},
    {"is_file", fn_is_file},
    {"mkdir", fn_mkdir},
    {"monotonic_ms", fn_monotonic_ms},
    {"now", fn_now},
    {"os_name", fn_os_name},
    {"path_basename", fn_path_basename},
    {"path_dirname", fn_path_dirname},
    {"path_is_absolute", fn_path_is_absolute},
    {"path_join", fn_path_join},
    {"pid", fn_pid},
    {"read_file", fn_read_file},
    {"rmdir", fn_rmdir},
    {"tmpdir", fn_tmpdir},
    {"uptime_ms", fn_uptime_ms},
    {"write_file", fn_write_file},
};

static const exprtk_module_t io_module = {"io", io_entries,
                                          sizeof(io_entries) / sizeof(io_entries[0])};

const exprtk_module_t *exprtk_module_io(void) { return &io_module; }
