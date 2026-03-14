#ifndef ROUTER_H
#define ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "turbo_buffer.h"
#include "request.h"
#include "platform.h"
#include "CoroNet/turbo_coro_socket.h"
#include "security.h"

#ifdef __cplusplus
extern "C" {
#endif

// HTTP Status Codes
typedef enum {
  // 1xx Informational
  CONTINUE = 100,
  SWITCHING_PROTOCOLS = 101,
  PROCESSING = 102,
  EARLY_HINTS = 103,

  // 2xx Success
  OK = 200,
  CREATED = 201,
  ACCEPTED = 202,
  NON_AUTHORITATIVE_INFORMATION = 203,
  NO_CONTENT = 204,
  RESET_CONTENT = 205,
  PARTIAL_CONTENT = 206,
  MULTI_STATUS = 207,
  ALREADY_REPORTED = 208,
  IM_USED = 226,

  // 3xx Redirection
  MULTIPLE_CHOICES = 300,
  MOVED_PERMANENTLY = 301,
  FOUND = 302,
  SEE_OTHER = 303,
  NOT_MODIFIED = 304,
  USE_PROXY = 305,
  TEMPORARY_REDIRECT = 307,
  PERMANENT_REDIRECT = 308,

  // 4xx Client Error
  BAD_REQUEST = 400,
  UNAUTHORIZED = 401,
  PAYMENT_REQUIRED = 402,
  FORBIDDEN = 403,
  NOT_FOUND = 404,
  METHOD_NOT_ALLOWED = 405,
  NOT_ACCEPTABLE = 406,
  PROXY_AUTHENTICATION_REQUIRED = 407,
  REQUEST_TIMEOUT = 408,
  CONFLICT = 409,
  GONE = 410,
  LENGTH_REQUIRED = 411,
  PRECONDITION_FAILED = 412,
  PAYLOAD_TOO_LARGE = 413,
  URI_TOO_LONG = 414,
  UNSUPPORTED_MEDIA_TYPE = 415,
  RANGE_NOT_SATISFIABLE = 416,
  EXPECTATION_FAILED = 417,
  IM_A_TEAPOT = 418,
  MISDIRECTED_REQUEST = 421,
  UNPROCESSABLE_ENTITY = 422,
  LOCKED = 423,
  FAILED_DEPENDENCY = 424,
  TOO_EARLY = 425,
  UPGRADE_REQUIRED = 426,
  PRECONDITION_REQUIRED = 428,
  TOO_MANY_REQUESTS = 429,
  REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
  UNAVAILABLE_FOR_LEGAL_REASONS = 451,

  // 5xx Server Error
  INTERNAL_SERVER_ERROR = 500,
  NOT_IMPLEMENTED = 501,
  BAD_GATEWAY = 502,
  SERVICE_UNAVAILABLE = 503,
  GATEWAY_TIMEOUT = 504,
  HTTP_VERSION_NOT_SUPPORTED = 505,
  VARIANT_ALSO_NEGOTIATES = 506,
  INSUFFICIENT_STORAGE = 507,
  LOOP_DETECTED = 508,
  NOT_EXTENDED = 510,
  NETWORK_AUTHENTICATION_REQUIRED = 511
} http_status_t;

// Arena-aware context structure for middleware data
typedef struct {
  void *data;
  size_t size;
  void (*cleanup)(void *data);
  mem_pool_t *arena; /* Phase IRIS-1: Changed from Arena* */
} req_context_t;

// Arena-aware Request structure
typedef struct Req {
  struct iris_app *app; /* Pointer to the application instance */
  mem_pool_t *arena; /* Phase IRIS-1: Pointer to shared request/response arena */
  coro_socket_t *client; /* Coroutine client connection */
  char *method;
  char *path;
  char *body;
  size_t body_len;
  request_t headers;
  request_t query;
  request_t params;
  req_context_t context; // Middleware context
  
  // Security context for tracking validation state and security flags
  iris_security_context_t *security;
  time_t request_start_time;
} Req;

// HTTP Header structure
typedef struct {
  char *name;
  char *value;
} http_header_t;

// Arena-aware Response structure
typedef struct Res {
  mem_pool_t *arena; /* Phase IRIS-1: Pointer to shared request/response arena */
  coro_socket_t *client; /* Coroutine client connection */
  int status;
  char *content_type; // Arena allocated string
  void *body;         // Arena allocated if owned by Res
  size_t body_len;
  int keep_alive;
  http_header_t *headers; // Arena allocated array
  int header_count;
  int header_capacity;
} Res;

// Write request structure (opaque, defined in router.c)
typedef struct write_req_s write_req_t;

// Route handler function type
typedef void (*RequestHandler)(Req *req, Res *res);

// Route definition
typedef struct {
  const char *method;
  const char *path;
  RequestHandler handler;
  void *middleware_ctx;
} Router;

// Forward declarations
typedef struct MiddlewareInfo MiddlewareInfo;
struct iris_app;
CXX_C_API void execute_middleware_chain(Req *req, Res *res, MiddlewareInfo *middleware_info);

// Function declarations
CXX_C_API int iris_app_execute(struct iris_app *app, coro_socket_t *client, const char *request_data, size_t request_len);
CXX_C_API int router(coro_socket_t *client, const char *request_data, size_t request_len);
CXX_C_API Req *arena_copy_req(mem_pool_t *target_arena, const Req *original);  /* Phase IRIS-1: Updated param type */
CXX_C_API Res *arena_copy_res(mem_pool_t *target_arena, const Res *original);  /* Phase IRIS-1: Updated param type */
CXX_C_API Req *copy_req(const Req *original);
CXX_C_API Res *copy_res(const Res *original);
CXX_C_API void destroy_req(Req *req);
CXX_C_API void destroy_res(Res *res);

CXX_C_API void set_header(Res *res, const char *name, const char *value);
CXX_C_API void reply(Res *res, int status, const char *content_type, const void *body, size_t body_len);

// Context management functions
CXX_C_API void set_context(Req *req, void *data, size_t size, void (*cleanup)(void *));
CXX_C_API void *get_context(Req *req);

// Connection context management functions
CXX_C_API void set_connection_context(coro_socket_t *client, void *data, void (*cleanup)(void *));
CXX_C_API void *get_connection_context(coro_socket_t *client);

// Convenience response functions
static inline void send_text(Res *res, int status, const char *body) {
  reply(res, status, "text/plain", body, strlen(body));
}

static inline void send_html(Res *res, int status, const char *body) {
  reply(res, status, "text/html", body, strlen(body));
}

static inline void send_json(Res *res, int status, const char *body) {
  reply(res, status, "application/json", body, strlen(body));
}



// Streaming (SSE) support
CXX_C_API void reply_stream_start(Res *res, int status);
CXX_C_API void reply_stream_chunk(Res *res, const char *data);
CXX_C_API void reply_stream_end(Res *res);

// =============================================================================
// File Download API
// =============================================================================

/**
 * @brief Send entire file as response (Content-Length mode)
 *
 * @param res Response object
 * @param status HTTP status code (usually 200)
 * @param content_type MIME type (e.g., "application/octet-stream")
 * @param file_path Path to file
 * @return 0 on success, -1 on error (file not found, etc.)
 *
 * @note Reads entire file into memory. For large files, use chunked API.
 */
CXX_C_API int reply_file(Res *res, int status, const char *content_type, const char *file_path);

/**
 * @brief Send file with Content-Disposition header for download
 *
 * @param res Response object
 * @param file_path Path to file
 * @param download_name Filename shown in browser download dialog (NULL = use original)
 * @return 0 on success, -1 on error
 */
CXX_C_API int reply_download(Res *res, const char *file_path, const char *download_name);

/**
 * @brief Start chunked transfer response
 *
 * @param res Response object
 * @param status HTTP status code
 * @param content_type MIME type
 */
CXX_C_API void reply_chunked_start(Res *res, int status, const char *content_type);

/**
 * @brief Send a chunk of data
 *
 * @param res Response object
 * @param data Data to send
 * @param len Length of data
 */
CXX_C_API void reply_chunked_write(Res *res, const void *data, size_t len);

/**
 * @brief End chunked transfer
 *
 * @param res Response object
 */
CXX_C_API void reply_chunked_end(Res *res);

/**
 * @brief Send file using chunked transfer (for large files)
 *
 * @param res Response object
 * @param status HTTP status code
 * @param content_type MIME type
 * @param file_path Path to file
 * @param chunk_size Size of each chunk (0 = default 64KB)
 * @return 0 on success, -1 on error
 */
CXX_C_API int reply_file_chunked(Res *res, int status, const char *content_type,
                                  const char *file_path, size_t chunk_size);

// Convenience getter functions
static inline const char *get_params(const Req *req, const char *key) {
  return get_req(&req->params, key);
}

static inline const char *get_query(const Req *req, const char *key) {
  return get_req(&req->query, key);
}

static inline const char *get_headers(const Req *req, const char *key) {
  return get_req(&req->headers, key);
}

#ifdef __cplusplus
}
#endif

#endif
