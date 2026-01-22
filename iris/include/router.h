#ifndef ROUTER_H
#define ROUTER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Phase IRIS-1: Use turbo_arena instead of vendor arena for consistency with MQTT/HTTP */
#include "arena_buffer.h"
#include "request.h"
#include "platform.h"
#include "netcore/turbo_async_server.h"
#include "security.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phase IRIS-1: Update macros to use turbo_arena API */
#define ecewo_alloc(req, size) turbo_arena_alloc(&(req)->arena, (size))
#define ecewo_strdup(req, str) turbo_arena_strdup(&(req)->arena, (str))
/* Note: turbo_arena doesn't have sprintf - will handle separately in rpc.c */
/* #define ecewo_sprintf(req, fmt, ...) turbo_arena_sprintf(&(req)->arena, (fmt), ##__VA_ARGS__) */

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
  turbo_arena_t *arena; /* Phase IRIS-1: Changed from Arena* */
} req_context_t;

// Arena-aware Request structure
typedef struct Req {
  turbo_arena_t *arena; /* Phase IRIS-1: Pointer to shared request/response arena */
  async_server_connection_t *connection; /* NetCore migration: replaced uv_tcp_t* */
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
  turbo_arena_t *arena; /* Phase IRIS-1: Pointer to shared request/response arena */
  async_server_connection_t *connection; /* NetCore migration: replaced uv_tcp_t* */
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

// Forward declaration from middleware.ch
typedef struct MiddlewareInfo MiddlewareInfo;
void execute_middleware_chain(Req *req, Res *res, MiddlewareInfo *middleware_info);

// Function declarations
CXX_C_API int router(async_server_connection_t *connection, const char *request_data, size_t request_len);
CXX_C_API Req *arena_copy_req(turbo_arena_t *target_arena, const Req *original);  /* Phase IRIS-1: Updated param type */
CXX_C_API Res *arena_copy_res(turbo_arena_t *target_arena, const Res *original);  /* Phase IRIS-1: Updated param type */
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
CXX_C_API void set_connection_context(async_server_connection_t *connection, void *data, void (*cleanup)(void *));
CXX_C_API void *get_connection_context(async_server_connection_t *connection);

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

static inline void send_cbor(Res *res, int status, const char *body, size_t body_len) {
  reply(res, status, "application/cbor", body, body_len);
}

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

#endif
