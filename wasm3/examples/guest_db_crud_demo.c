#include <stdint.h>

__attribute__((import_module("TurboUtils"), import_name("db_open")))
extern int32_t turboutils_db_open(const char *target, uint32_t target_len,
                                uint32_t *out_handle);

__attribute__((import_module("TurboUtils"), import_name("db_close")))
extern int32_t turboutils_db_close(uint32_t handle);

__attribute__((import_module("TurboUtils"), import_name("db_exec")))
extern int32_t turboutils_db_exec(uint32_t handle, const char *sql, uint32_t sql_len,
                                uint64_t *out_changes);

__attribute__((import_module("TurboUtils"), import_name("db_prepare")))
extern int32_t turboutils_db_prepare(uint32_t db_handle, const char *sql,
                                   uint32_t sql_len, uint32_t *out_stmt);

__attribute__((import_module("TurboUtils"), import_name("db_bind_i64")))
extern int32_t turboutils_db_bind_i64(uint32_t stmt_handle, uint32_t index,
                                    int64_t value);

__attribute__((import_module("TurboUtils"), import_name("db_bind_text")))
extern int32_t turboutils_db_bind_text(uint32_t stmt_handle, uint32_t index,
                                     const char *text, uint32_t text_len);

__attribute__((import_module("TurboUtils"), import_name("db_step")))
extern int32_t turboutils_db_step(uint32_t stmt_handle, int32_t *out_state);

__attribute__((import_module("TurboUtils"), import_name("db_column_i64")))
extern int32_t turboutils_db_column_i64(uint32_t stmt_handle, uint32_t index,
                                      int64_t *out_value);

__attribute__((import_module("TurboUtils"), import_name("db_column_text")))
extern int32_t turboutils_db_column_text(uint32_t stmt_handle, uint32_t index,
                                       char *buffer, uint32_t buffer_size,
                                       uint32_t *out_written);

__attribute__((import_module("TurboUtils"), import_name("db_reset")))
extern int32_t turboutils_db_reset(uint32_t stmt_handle);

__attribute__((import_module("TurboUtils"), import_name("db_finalize")))
extern int32_t turboutils_db_finalize(uint32_t stmt_handle);

enum {
  TURBO_WASM3_DB_STEP_DONE = 0,
  TURBO_WASM3_DB_STEP_ROW = 1,
};

static uint32_t cstrlen(const char *s) {
  uint32_t len = 0;
  while (s[len] != '\0') {
    ++len;
  }
  return len;
}

static int cstreq(const char *a, const char *b) {
  uint32_t i = 0;
  while (a[i] != '\0' && b[i] != '\0') {
    if (a[i] != b[i]) {
      return 0;
    }
    ++i;
  }
  return a[i] == b[i];
}

__attribute__((export_name("run_db_crud_demo")))
int32_t run_db_crud_demo(void) {
  static const char target[] = ":memory:";
  static const char create_sql[] =
      "create table users(id integer primary key, name text);";
  static const char insert_sql[] = "insert into users(id, name) values(?, ?);";
  static const char select_sql[] = "select name from users where id = ?;";
  static const char update_sql[] = "update users set name = ? where id = ?;";
  static const char delete_sql[] = "delete from users where id = ?;";
  static const char count_sql[] = "select count(*) from users;";
  static const char alpha[] = "alpha";
  static const char beta[] = "beta";
  uint32_t db = 0;
  uint32_t stmt = 0;
  uint64_t changes = 0;
  int32_t step_state = TURBO_WASM3_DB_STEP_DONE;
  int64_t count_value = 0;
  uint32_t name_len = 0;
  char name_buf[16];
  int32_t rc = 0;

  rc = turboutils_db_open(target, cstrlen(target), &db);
  if (rc != 0) {
    return 10 + rc;
  }

  rc = turboutils_db_exec(db, create_sql, cstrlen(create_sql), &changes);
  if (rc != 0 || changes != 0) {
    return 20 + rc;
  }

  rc = turboutils_db_prepare(db, insert_sql, cstrlen(insert_sql), &stmt);
  if (rc != 0) {
    return 30 + rc;
  }
  rc = turboutils_db_bind_i64(stmt, 1, 1);
  if (rc != 0) {
    return 40 + rc;
  }
  rc = turboutils_db_bind_text(stmt, 2, alpha, cstrlen(alpha));
  if (rc != 0) {
    return 50 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_DONE) {
    return 60 + rc;
  }
  rc = turboutils_db_finalize(stmt);
  if (rc != 0) {
    return 70 + rc;
  }
  stmt = 0;

  rc = turboutils_db_prepare(db, select_sql, cstrlen(select_sql), &stmt);
  if (rc != 0) {
    return 80 + rc;
  }
  rc = turboutils_db_bind_i64(stmt, 1, 1);
  if (rc != 0) {
    return 90 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_ROW) {
    return 100 + rc;
  }
  rc = turboutils_db_column_text(stmt, 0, name_buf, sizeof(name_buf), &name_len);
  if (rc != 0 || name_len != cstrlen(alpha) || !cstreq(name_buf, alpha)) {
    return 110 + rc;
  }
  rc = turboutils_db_finalize(stmt);
  if (rc != 0) {
    return 120 + rc;
  }
  stmt = 0;

  rc = turboutils_db_prepare(db, update_sql, cstrlen(update_sql), &stmt);
  if (rc != 0) {
    return 130 + rc;
  }
  rc = turboutils_db_bind_text(stmt, 1, beta, cstrlen(beta));
  if (rc != 0) {
    return 140 + rc;
  }
  rc = turboutils_db_bind_i64(stmt, 2, 1);
  if (rc != 0) {
    return 150 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_DONE) {
    return 160 + rc;
  }
  rc = turboutils_db_finalize(stmt);
  if (rc != 0) {
    return 170 + rc;
  }
  stmt = 0;

  rc = turboutils_db_prepare(db, select_sql, cstrlen(select_sql), &stmt);
  if (rc != 0) {
    return 180 + rc;
  }
  rc = turboutils_db_bind_i64(stmt, 1, 1);
  if (rc != 0) {
    return 190 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_ROW) {
    return 200 + rc;
  }
  rc = turboutils_db_column_text(stmt, 0, name_buf, sizeof(name_buf), &name_len);
  if (rc != 0 || name_len != cstrlen(beta) || !cstreq(name_buf, beta)) {
    return 210 + rc;
  }
  rc = turboutils_db_reset(stmt);
  if (rc != 0) {
    return 220 + rc;
  }
  rc = turboutils_db_bind_i64(stmt, 1, 99);
  if (rc != 0) {
    return 230 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_DONE) {
    return 240 + rc;
  }
  rc = turboutils_db_finalize(stmt);
  if (rc != 0) {
    return 250 + rc;
  }
  stmt = 0;

  rc = turboutils_db_prepare(db, delete_sql, cstrlen(delete_sql), &stmt);
  if (rc != 0) {
    return 260 + rc;
  }
  rc = turboutils_db_bind_i64(stmt, 1, 1);
  if (rc != 0) {
    return 270 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_DONE) {
    return 280 + rc;
  }
  rc = turboutils_db_finalize(stmt);
  if (rc != 0) {
    return 290 + rc;
  }
  stmt = 0;

  rc = turboutils_db_prepare(db, count_sql, cstrlen(count_sql), &stmt);
  if (rc != 0) {
    return 300 + rc;
  }
  rc = turboutils_db_step(stmt, &step_state);
  if (rc != 0 || step_state != TURBO_WASM3_DB_STEP_ROW) {
    return 310 + rc;
  }
  rc = turboutils_db_column_i64(stmt, 0, &count_value);
  if (rc != 0 || count_value != 0) {
    return 320 + rc;
  }
  rc = turboutils_db_finalize(stmt);
  if (rc != 0) {
    return 330 + rc;
  }

  rc = turboutils_db_close(db);
  if (rc != 0) {
    return 340 + rc;
  }

  return 11;
}
