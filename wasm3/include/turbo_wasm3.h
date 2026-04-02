#ifndef TURBO_WASM3_H
#define TURBO_WASM3_H

#include "platform.h"
#include "m3_api_wasi.h"
#include "wasm3.h"
#include "turbo_parser.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct coro_socket_s;
struct m3_wasi_context_t;

typedef struct turbo_wasm3_socket_registry_s turbo_wasm3_socket_registry_t;
typedef struct turbo_wasm3_db_registry_s turbo_wasm3_db_registry_t;
typedef struct turbo_wasm3_http_registry_s turbo_wasm3_http_registry_t;
typedef struct turbo_wasm3_parser_registry_s turbo_wasm3_parser_registry_t;
typedef struct turbo_wasm3_vm_s turbo_wasm3_vm_t;
typedef M3Result (*turbo_wasm3_host_linker_fn)(turbo_wasm3_vm_t *vm,
                                               IM3Module module,
                                               void *user_data);

typedef struct turbo_wasm3_db_ops_s {
  int (*open)(void *user_data, const char *target, void **out_db);
  int (*close)(void *user_data, void *db);
  int (*exec)(void *user_data, void *db, const char *sql, uint64_t *changes);
  int (*error)(void *user_data, void *db, char *buffer, size_t buffer_size,
               uint32_t *out_len);
  int (*prepare)(void *user_data, void *db, const char *sql, void **out_stmt);
  int (*finalize)(void *user_data, void *stmt);
  int (*reset)(void *user_data, void *stmt);
  int (*step)(void *user_data, void *stmt, int32_t *out_state);
  int (*bind_int64)(void *user_data, void *stmt, uint32_t index, int64_t value);
  int (*bind_double)(void *user_data, void *stmt, uint32_t index, double value);
  int (*bind_null)(void *user_data, void *stmt, uint32_t index);
  int (*bind_blob)(void *user_data, void *stmt, uint32_t index, const void *value,
                   size_t value_len);
  int (*bind_text)(void *user_data, void *stmt, uint32_t index, const char *value,
                   size_t value_len);
  int (*column_type)(void *user_data, void *stmt, uint32_t index, int32_t *out_type);
  int (*column_int64)(void *user_data, void *stmt, uint32_t index,
                      int64_t *out_value);
  int (*column_double)(void *user_data, void *stmt, uint32_t index,
                       double *out_value);
  int (*column_blob)(void *user_data, void *stmt, uint32_t index, void *buffer,
                     size_t buffer_size, uint32_t *out_len);
  int (*column_text)(void *user_data, void *stmt, uint32_t index, char *buffer,
                     size_t buffer_size, uint32_t *out_len);
} turbo_wasm3_db_ops_t;

#define TURBO_WASM3_HOST_ABI_VERSION 2u
#define TURBO_WASM3_DB_STEP_DONE 0
#define TURBO_WASM3_DB_STEP_ROW 1
#define TURBO_WASM3_DB_TYPE_NULL 0
#define TURBO_WASM3_DB_TYPE_INT64 1
#define TURBO_WASM3_DB_TYPE_DOUBLE 2
#define TURBO_WASM3_DB_TYPE_TEXT 3
#define TURBO_WASM3_DB_TYPE_BLOB 4
#define TURBO_WASM3_HTTP_METHOD_DELETE 0
#define TURBO_WASM3_HTTP_METHOD_GET 1
#define TURBO_WASM3_HTTP_METHOD_HEAD 2
#define TURBO_WASM3_HTTP_METHOD_POST 3
#define TURBO_WASM3_HTTP_METHOD_PUT 4
#define TURBO_WASM3_HTTP_METHOD_OPTIONS 6
#define TURBO_WASM3_HTTP_METHOD_PATCH 28

/*
 * Create a TurboUtils-managed wasm3 VM.
 * The VM owns its environment, runtime, copied wasm bytes, and socket registry.
 */
CXX_C_API turbo_wasm3_vm_t *
turbo_wasm3_vm_create(uint32_t stack_size, void *runtime_user_data,
                      size_t socket_capacity);

CXX_C_API void
turbo_wasm3_vm_destroy(turbo_wasm3_vm_t *vm);

CXX_C_API IM3Runtime
turbo_wasm3_vm_get_runtime(turbo_wasm3_vm_t *vm);

CXX_C_API m3_wasi_context_t *
turbo_wasm3_vm_get_wasi_context(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_db_registry_t *
turbo_wasm3_vm_get_db_registry(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_http_registry_t *
turbo_wasm3_vm_get_http_registry(turbo_wasm3_vm_t *vm);

CXX_C_API turbo_wasm3_parser_registry_t *
turbo_wasm3_vm_get_parser_registry(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_vm_set_wasi_args(turbo_wasm3_vm_t *vm, uint32_t argc,
                             const char *const *argv);

CXX_C_API void
turbo_wasm3_vm_reset_preopens(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_vm_set_preopen(turbo_wasm3_vm_t *vm, uint32_t fd,
                           const char *guest_path, const char *host_path);

CXX_C_API int
turbo_wasm3_vm_remove_preopen(turbo_wasm3_vm_t *vm, uint32_t fd);

CXX_C_API M3Result
turbo_wasm3_vm_load_module(turbo_wasm3_vm_t *vm, const uint8_t *wasm_bytes,
                           uint32_t wasm_size, const char *module_name,
                           IM3Module *out_module);

CXX_C_API M3Result
turbo_wasm3_vm_load_module_file(turbo_wasm3_vm_t *vm, const char *path,
                                const char *module_name, IM3Module *out_module);

/*
 * Host linkers let TurboUtils bind stable host imports without coupling guests to
 * backend libraries directly. Add them before loading a module.
 */
CXX_C_API void
turbo_wasm3_vm_set_host_user_data(turbo_wasm3_vm_t *vm, void *host_user_data);

CXX_C_API void *
turbo_wasm3_vm_get_host_user_data(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_vm_add_host_linker(turbo_wasm3_vm_t *vm,
                               turbo_wasm3_host_linker_fn linker,
                               void *user_data);

CXX_C_API void
turbo_wasm3_vm_clear_host_linkers(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_host_modules(turbo_wasm3_vm_t *vm, IM3Module module);

/*
 * Built-in TurboUtils host module:
 *   import "TurboUtils" "abi_version"    : i32 () -> ABI version
 *   import "TurboUtils" "clock_time_ms"  : i64 () -> monotonic-ish host time in ms
 *   import "TurboUtils" "socket_send"    : i32 (fd, ptr, len, out_sent)
 *   import "TurboUtils" "socket_recv"    : i32 (fd, ptr, len, out_recv)
 *   import "TurboUtils" "socket_release" : i32 (fd)
 *   import "TurboUtils" "db_open"        : i32 (ptr, len, out_handle)
 *   import "TurboUtils" "db_close"       : i32 (handle)
 *   import "TurboUtils" "db_exec"        : i32 (handle, ptr, len, out_changes)
 *   import "TurboUtils" "db_error"       : i32 (handle, ptr, len, out_written)
 *   import "TurboUtils" "db_stmt_error"  : i32 (stmt, ptr, len, out_written)
 *   import "TurboUtils" "db_prepare"     : i32 (db, ptr, len, out_stmt)
 *   import "TurboUtils" "db_bind_i64"    : i32 (stmt, index, value)
 *   import "TurboUtils" "db_bind_f64"    : i32 (stmt, index, value)
 *   import "TurboUtils" "db_bind_null"   : i32 (stmt, index)
 *   import "TurboUtils" "db_bind_blob"   : i32 (stmt, index, ptr, len)
 *   import "TurboUtils" "db_bind_text"   : i32 (stmt, index, ptr, len)
 *   import "TurboUtils" "db_step"        : i32 (stmt, out_state)
 *   import "TurboUtils" "db_column_type" : i32 (stmt, index, out_type)
 *   import "TurboUtils" "db_column_i64"  : i32 (stmt, index, out_value)
 *   import "TurboUtils" "db_column_f64"  : i32 (stmt, index, out_value)
 *   import "TurboUtils" "db_column_blob" : i32 (stmt, index, ptr, len, out_written)
 *   import "TurboUtils" "db_column_text" : i32 (stmt, index, ptr, len, out_written)
 *   import "TurboUtils" "db_reset"       : i32 (stmt)
 *   import "TurboUtils" "db_finalize"    : i32 (stmt)
 */
CXX_C_API int
turbo_wasm3_vm_enable_turboutils_host(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_turboutils_host(turbo_wasm3_vm_t *vm, IM3Module module);

/*
 * Optional TurboUtils HTTP host module, also linked under "TurboUtils":
 *   import "TurboUtils" "http_client_open"       : i32 (ptr, len, out_client)
 *   import "TurboUtils" "http_client_close"      : i32 (client)
 *   import "TurboUtils" "http_client_set_timeout": i32 (client, timeout_ms)
 *   import "TurboUtils" "http_client_set_default_header": i32 (client, name_ptr, name_len, value_ptr, value_len)
 *   import "TurboUtils" "http_client_remove_default_header": i32 (client, name_ptr, name_len)
 *   import "TurboUtils" "http_client_clear_default_headers": i32 (client)
 *   import "TurboUtils" "http_client_set_basic_auth": i32 (client, user_ptr, user_len, pass_ptr, pass_len)
 *   import "TurboUtils" "http_client_set_bearer_token": i32 (client, token_ptr, token_len)
 *   import "TurboUtils" "http_client_clear_auth": i32 (client)
 *   import "TurboUtils" "http_client_set_proxy": i32 (client, host_ptr, host_len, port, user_ptr, user_len, pass_ptr, pass_len)
 *   import "TurboUtils" "http_client_clear_proxy": i32 (client)
 *   import "TurboUtils" "http_request"           : i32 (client, method, url_ptr, url_len, body_ptr, body_len, out_resp)
 *   import "TurboUtils" "http_request_with_headers": i32 (client, method, url_ptr, url_len, headers_ptr, headers_len, body_ptr, body_len, out_resp)
 *   import "TurboUtils" "http_stream_get"        : i32 (client, url_ptr, url_len, out_resp)
 *   import "TurboUtils" "http_sse_get"           : i32 (client, url_ptr, url_len, out_resp)
 *   import "TurboUtils" "http_response_status"   : i32 (resp, out_status)
 *   import "TurboUtils" "http_response_error_code": i32 (resp, out_code)
 *   import "TurboUtils" "http_response_is_sse"   : i32 (resp, out_flag)
 *   import "TurboUtils" "http_response_chunk_count": i32 (resp, out_count)
 *   import "TurboUtils" "http_response_chunk"    : i32 (resp, index, ptr, len, out_written)
 *   import "TurboUtils" "http_response_header"   : i32 (resp, name_ptr, name_len, ptr, len, out_written)
 *   import "TurboUtils" "http_response_headers"  : i32 (resp, ptr, len, out_written)
 *   import "TurboUtils" "http_response_body"     : i32 (resp, ptr, len, out_written)
 *   import "TurboUtils" "http_response_error"    : i32 (resp, ptr, len, out_written)
 *   import "TurboUtils" "http_response_close"    : i32 (resp)
 */
CXX_C_API int
turbo_wasm3_vm_enable_http_host(turbo_wasm3_vm_t *vm);

CXX_C_API M3Result
turbo_wasm3_vm_link_http_host(turbo_wasm3_vm_t *vm, IM3Module module);

/*
 * Registered sockets are borrowed, not owned.
 * Execute wasm3 from the same CoroNet scheduling context that owns them.
 */
CXX_C_API turbo_wasm3_socket_registry_t *
turbo_wasm3_socket_registry_create(size_t initial_capacity);

CXX_C_API void
turbo_wasm3_socket_registry_destroy(turbo_wasm3_socket_registry_t *registry);

CXX_C_API int
turbo_wasm3_socket_registry_bind_wasi(turbo_wasm3_socket_registry_t *registry,
                                      struct m3_wasi_context_t *wasi_context);

CXX_C_API int
turbo_wasm3_socket_registry_unbind_wasi(struct m3_wasi_context_t *wasi_context);

CXX_C_API int
turbo_wasm3_socket_registry_register(turbo_wasm3_socket_registry_t *registry,
                                     struct coro_socket_s *socket,
                                     uint32_t *wasi_fd);

CXX_C_API int
turbo_wasm3_socket_registry_unregister(turbo_wasm3_socket_registry_t *registry,
                                       uint32_t wasi_fd);

CXX_C_API struct coro_socket_s *
turbo_wasm3_socket_registry_lookup(turbo_wasm3_socket_registry_t *registry,
                                   uint32_t wasi_fd);

CXX_C_API int
turbo_wasm3_vm_register_socket(turbo_wasm3_vm_t *vm, struct coro_socket_s *socket,
                               uint32_t *wasi_fd);

CXX_C_API turbo_wasm3_db_registry_t *
turbo_wasm3_db_registry_create(size_t initial_capacity);

CXX_C_API void
turbo_wasm3_db_registry_destroy(turbo_wasm3_db_registry_t *registry);

CXX_C_API int
turbo_wasm3_db_registry_set_ops(turbo_wasm3_db_registry_t *registry,
                                const turbo_wasm3_db_ops_t *ops,
                                void *user_data);

CXX_C_API int
turbo_wasm3_db_registry_enable_sqlite(turbo_wasm3_db_registry_t *registry);

CXX_C_API int
turbo_wasm3_vm_enable_sqlite_db(turbo_wasm3_vm_t *vm);

CXX_C_API int
turbo_wasm3_db_registry_open(turbo_wasm3_db_registry_t *registry,
                             const char *target, uint32_t *db_handle);

CXX_C_API int
turbo_wasm3_db_registry_close(turbo_wasm3_db_registry_t *registry,
                              uint32_t db_handle);

CXX_C_API int
turbo_wasm3_db_registry_exec(turbo_wasm3_db_registry_t *registry,
                             uint32_t db_handle, const char *sql,
                             uint64_t *changes);

CXX_C_API int
turbo_wasm3_db_registry_prepare(turbo_wasm3_db_registry_t *registry,
                                uint32_t db_handle, const char *sql,
                                uint32_t *stmt_handle);

CXX_C_API int
turbo_wasm3_db_registry_finalize(turbo_wasm3_db_registry_t *registry,
                                 uint32_t stmt_handle);

CXX_C_API int
turbo_wasm3_db_registry_reset(turbo_wasm3_db_registry_t *registry,
                              uint32_t stmt_handle);

CXX_C_API int
turbo_wasm3_db_registry_step(turbo_wasm3_db_registry_t *registry,
                             uint32_t stmt_handle, int32_t *out_state);

CXX_C_API int
turbo_wasm3_db_registry_bind_int64(turbo_wasm3_db_registry_t *registry,
                                   uint32_t stmt_handle, uint32_t index,
                                   int64_t value);

CXX_C_API int
turbo_wasm3_db_registry_bind_double(turbo_wasm3_db_registry_t *registry,
                                    uint32_t stmt_handle, uint32_t index,
                                    double value);

CXX_C_API int
turbo_wasm3_db_registry_bind_null(turbo_wasm3_db_registry_t *registry,
                                  uint32_t stmt_handle, uint32_t index);

CXX_C_API int
turbo_wasm3_db_registry_bind_blob(turbo_wasm3_db_registry_t *registry,
                                  uint32_t stmt_handle, uint32_t index,
                                  const void *value, size_t value_len);

CXX_C_API int
turbo_wasm3_db_registry_bind_text(turbo_wasm3_db_registry_t *registry,
                                  uint32_t stmt_handle, uint32_t index,
                                  const char *value, size_t value_len);

CXX_C_API int
turbo_wasm3_db_registry_column_type(turbo_wasm3_db_registry_t *registry,
                                    uint32_t stmt_handle, uint32_t index,
                                    int32_t *out_type);

CXX_C_API int
turbo_wasm3_db_registry_column_int64(turbo_wasm3_db_registry_t *registry,
                                     uint32_t stmt_handle, uint32_t index,
                                     int64_t *out_value);

CXX_C_API int
turbo_wasm3_db_registry_column_double(turbo_wasm3_db_registry_t *registry,
                                      uint32_t stmt_handle, uint32_t index,
                                      double *out_value);

CXX_C_API int
turbo_wasm3_db_registry_column_blob(turbo_wasm3_db_registry_t *registry,
                                    uint32_t stmt_handle, uint32_t index,
                                    void *buffer, size_t buffer_size,
                                    uint32_t *out_len);

CXX_C_API int
turbo_wasm3_db_registry_column_text(turbo_wasm3_db_registry_t *registry,
                                    uint32_t stmt_handle, uint32_t index,
                                    char *buffer, size_t buffer_size,
                                    uint32_t *out_len);

CXX_C_API int
turbo_wasm3_db_registry_error(turbo_wasm3_db_registry_t *registry,
                              uint32_t db_handle, char *buffer,
                              size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_db_registry_stmt_error(turbo_wasm3_db_registry_t *registry,
                                   uint32_t stmt_handle, char *buffer,
                                   size_t buffer_size, uint32_t *out_len);

CXX_C_API turbo_wasm3_http_registry_t *
turbo_wasm3_http_registry_create(size_t initial_capacity);

CXX_C_API void
turbo_wasm3_http_registry_destroy(turbo_wasm3_http_registry_t *registry);

CXX_C_API int
turbo_wasm3_http_registry_open_client(turbo_wasm3_http_registry_t *registry,
                                      const char *base_url,
                                      uint32_t *client_handle);

CXX_C_API int
turbo_wasm3_http_registry_close_client(turbo_wasm3_http_registry_t *registry,
                                       uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_set_timeout(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle, int timeout_ms);

CXX_C_API int
turbo_wasm3_http_registry_set_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name, const char *value);

CXX_C_API int
turbo_wasm3_http_registry_remove_default_header(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *name);

CXX_C_API int
turbo_wasm3_http_registry_clear_default_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_set_basic_auth(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *username, const char *password);

CXX_C_API int
turbo_wasm3_http_registry_set_bearer_token(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *token);

CXX_C_API int
turbo_wasm3_http_registry_clear_auth(turbo_wasm3_http_registry_t *registry,
                                     uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_set_proxy(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle,
    const char *host, uint16_t port, const char *username,
    const char *password);

CXX_C_API int
turbo_wasm3_http_registry_clear_proxy(turbo_wasm3_http_registry_t *registry,
                                      uint32_t client_handle);

CXX_C_API int
turbo_wasm3_http_registry_request(turbo_wasm3_http_registry_t *registry,
                                  uint32_t client_handle, int method,
                                  const char *url, const void *body,
                                  size_t body_len, uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_request_with_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t client_handle, int method,
    const char *url, const char *headers, size_t headers_len, const void *body,
    size_t body_len, uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_stream_get(turbo_wasm3_http_registry_t *registry,
                                     uint32_t client_handle, const char *url,
                                     uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_sse_get(turbo_wasm3_http_registry_t *registry,
                                  uint32_t client_handle, const char *url,
                                  uint32_t *response_handle);

CXX_C_API int
turbo_wasm3_http_registry_response_status(turbo_wasm3_http_registry_t *registry,
                                          uint32_t response_handle,
                                          int32_t *out_status);

CXX_C_API int
turbo_wasm3_http_registry_response_error_code(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_error_code);

CXX_C_API int
turbo_wasm3_http_registry_response_is_sse(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    int32_t *out_is_sse);

CXX_C_API int
turbo_wasm3_http_registry_response_chunk_count(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t *out_count);

CXX_C_API int
turbo_wasm3_http_registry_response_chunk(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    uint32_t chunk_index, void *buffer, size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_header(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle,
    const char *header_name, char *buffer, size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_headers(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_body(turbo_wasm3_http_registry_t *registry,
                                        uint32_t response_handle, void *buffer,
                                        size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_response_error(
    turbo_wasm3_http_registry_t *registry, uint32_t response_handle, char *buffer,
    size_t buffer_size, uint32_t *out_len);

CXX_C_API int
turbo_wasm3_http_registry_close_response(turbo_wasm3_http_registry_t *registry,
                                         uint32_t response_handle);

/*
 * Parser Registry - Unified handle-based parser for JSON/CSV/XML/INI/etc.
 * Gast sees only handles and ptr+len buffers.
 */
#define TURBO_WASM3_PARSER_TYPE_NONE 0
#define TURBO_WASM3_PARSER_TYPE_JSON 1
#define TURBO_WASM3_PARSER_TYPE_CSV 2
#define TURBO_WASM3_PARSER_TYPE_XML 3
#define TURBO_WASM3_PARSER_TYPE_INI 4
#define TURBO_WASM3_PARSER_TYPE_TOML 5
#define TURBO_WASM3_PARSER_TYPE_URI 6
#define TURBO_WASM3_PARSER_TYPE_DOTENV 7

CXX_C_API turbo_wasm3_vm_t *
turbo_wasm3_vm_get_parser_vm(turbo_wasm3_vm_t *vm);

/*
 * Parse functions - each returns a handle to parsed document.
 * import "TurboUtils" "parser_json_parse" : i32 (ptr, len, out_handle)
 * import "TurboUtils" "parser_csv_parse"  : i32 (ptr, len, out_handle)
 * import "TurboUtils" "parser_xml_parse"  : i32 (ptr, len, out_handle)
 * import "TurboUtils" "parser_ini_parse"  : i32 (ptr, len, out_handle)
 */
CXX_C_API int
turbo_wasm3_parser_parse_json(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

CXX_C_API int
turbo_wasm3_parser_parse_csv(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

CXX_C_API int
turbo_wasm3_parser_parse_xml(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

CXX_C_API int
turbo_wasm3_parser_parse_ini(turbo_wasm3_vm_t *vm, const uint8_t *data, size_t len, uint32_t *out_handle);

/*
 * Free parsed document.
 * import "TurboUtils" "parser_free" : i32 (handle)
 */
CXX_C_API int
turbo_wasm3_parser_free(turbo_wasm3_vm_t *vm, uint32_t handle);

/*
 * JSON accessors.
 * import "TurboUtils" "json_get_type"     : i32 (handle, path_ptr, path_len, out_type)
 * import "TurboUtils" "json_get_string"   : i32 (handle, path_ptr, path_len, buf, buf_len, out_written)
 * import "TurboUtils" "json_get_int"      : i32 (handle, path_ptr, path_len, out_value)
 * import "TurboUtils" "json_get_bool"     : i32 (handle, path_ptr, path_len, out_value)
 * import "TurboUtils" "json_get_double"   : i32 (handle, path_ptr, path_len, out_value)
 * import "TurboUtils" "json_array_size"   : i32 (handle, path_ptr, path_len, out_size)
 * import "TurboUtils" "json_object_keys"  : i32 (handle, path_ptr, path_len, buf, buf_len, out_written)
 */
CXX_C_API int
turbo_wasm3_json_get_type(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, int32_t *out_type);

CXX_C_API int
turbo_wasm3_json_get_string(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len,
                            char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_json_get_int(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, int32_t *out_value);

CXX_C_API int
turbo_wasm3_json_get_bool(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, int32_t *out_value);

CXX_C_API int
turbo_wasm3_json_get_double(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, double *out_value);

CXX_C_API int
turbo_wasm3_json_array_size(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len, uint32_t *out_size);

CXX_C_API int
turbo_wasm3_json_object_keys(turbo_wasm3_vm_t *vm, uint32_t handle, const char *path, size_t path_len,
                             char *buffer, size_t buffer_size, uint32_t *out_written);

/*
 * CSV accessors.
 * import "TurboUtils" "csv_row_count"   : i32 (handle, out_count)
 * import "TurboUtils" "csv_column_count": i32 (handle, out_count)
 * import "TurboUtils" "csv_get_cell"    : i32 (handle, row, col, buf, buf_len, out_written)
 * import "TurboUtils" "csv_find_column" : i32 (handle, name_ptr, name_len, out_col)
 */
CXX_C_API int
turbo_wasm3_csv_row_count(turbo_wasm3_vm_t *vm, uint32_t handle, uint32_t *out_count);

CXX_C_API int
turbo_wasm3_csv_column_count(turbo_wasm3_vm_t *vm, uint32_t handle, uint32_t *out_count);

CXX_C_API int
turbo_wasm3_csv_get_cell(turbo_wasm3_vm_t *vm, uint32_t handle, uint32_t row, uint32_t col,
                         char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_csv_find_column(turbo_wasm3_vm_t *vm, uint32_t handle, const char *name, size_t name_len, uint32_t *out_col);

/*
 * XML accessors.
 * import "TurboUtils" "xml_root_name"   : i32 (handle, buf, buf_len, out_written)
 * import "TurboUtils" "xml_get_text"    : i32 (handle, xpath_ptr, xpath_len, buf, buf_len, out_written)
 * import "TurboUtils" "xml_count"       : i32 (handle, xpath_ptr, xpath_len, out_count)
 */
CXX_C_API int
turbo_wasm3_xml_root_name(turbo_wasm3_vm_t *vm, uint32_t handle, char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_xml_get_text(turbo_wasm3_vm_t *vm, uint32_t handle, const char *xpath, size_t xpath_len,
                         char *buffer, size_t buffer_size, uint32_t *out_written);

CXX_C_API int
turbo_wasm3_xml_count(turbo_wasm3_vm_t *vm, uint32_t handle, const char *xpath, size_t xpath_len, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_WASM3_H */
