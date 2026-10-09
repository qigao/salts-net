#include "salts_tcp_proxy.h"

#include <base64_utils.h>
#include <salts/error_codes.h>
#include <cnet/destination_policy.h>
#include <cmeta_buffer.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int salts_proxy_cnet_send_bytes(cnet_client *client,
                                       cnet_connection connection,
                                       const void *data,
                                       size_t size,
                                       int close_after) {
  mem_buffer_t *buffer;
  int status;
  if (!client || !data || size == 0u) return SALTS_EINVAL;
  buffer = mem_get_buffer(mem_global(), size);
  if (!buffer) return SALTS_ENOMEM;
  memcpy(mem_buffer_data(buffer), data, size);
  mem_set_used(buffer, size);
  status = close_after
               ? cnet_send_buffer_and_close(client, connection, buffer)
               : cnet_send_buffer(client, connection, buffer);
  mem_buffer_release(buffer);
  return status;
}

enum {
  SALTS_PROXY_DEFAULT_SESSION_CAPACITY = 64,
  SALTS_PROXY_DEFAULT_HANDSHAKE_CAPACITY = 16 * 1024,
  SALTS_PROXY_DEFAULT_MAX_MESSAGE_BYTES = 64 * 1024,
  SALTS_PROXY_DEFAULT_COMMAND_CAPACITY = 256,
  SALTS_PROXY_DEFAULT_REQUEST_CAPACITY = 256,
  SALTS_PROXY_DEFAULT_EVENT_CAPACITY = 256,
  SALTS_PROXY_DEFAULT_COMPLETION_CAPACITY = 128,
  SALTS_PROXY_DEFAULT_BACKLOG = 64,
  SALTS_PROXY_DEFAULT_IO_TIMEOUT_MS = 30000,
  SALTS_PROXY_DEFAULT_SHUTDOWN_TIMEOUT_MS = 5000,
  SALTS_PROXY_HOST_CAPACITY = 254,
  SALTS_PROXY_URI_CAPACITY = 1281
};

static const unsigned char salts_socks_success[] = {5u, 0u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
static const char salts_http_success[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
static const char salts_http_bad_request[] =
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
static const char salts_http_auth_required[] =
    "HTTP/1.1 407 Proxy Authentication Required\r\n"
    "Proxy-Authenticate: Basic realm=\"SaltsNet\"\r\nConnection: close\r\n\r\n";
static const char salts_http_bad_gateway[] =
    "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";

typedef enum salts_proxy_phase {
  SALTS_PROXY_PHASE_FREE = 0,
  SALTS_PROXY_PHASE_ACCEPTED,
  SALTS_PROXY_PHASE_DENIED,
  SALTS_PROXY_PHASE_AUTO,
  SALTS_PROXY_PHASE_SOCKS_GREETING,
  SALTS_PROXY_PHASE_SOCKS_AUTH,
  SALTS_PROXY_PHASE_SOCKS_REQUEST,
  SALTS_PROXY_PHASE_HTTP,
  SALTS_PROXY_PHASE_CONNECTING,
  SALTS_PROXY_PHASE_REPLYING,
  SALTS_PROXY_PHASE_PUMP,
  SALTS_PROXY_PHASE_CLOSING
} salts_proxy_phase_t;

typedef enum salts_proxy_side_role {
  SALTS_PROXY_SIDE_DOWNSTREAM = 0,
  SALTS_PROXY_SIDE_UPSTREAM = 1
} salts_proxy_side_role_t;

typedef enum salts_proxy_send_action {
  SALTS_PROXY_SEND_NONE = 0,
  SALTS_PROXY_SEND_PROCESS_HANDSHAKE,
  SALTS_PROXY_SEND_START_PUMP,
  SALTS_PROXY_SEND_REARM_SOURCE
} salts_proxy_send_action_t;

struct salts_proxy_session;

typedef struct salts_proxy_side {
  struct salts_proxy_session *session;
  salts_proxy_side_role_t role;
  cnet_connection connection;
  int live;
  int connected;
  int receive_armed;
  salts_proxy_send_action_t send_action;
  struct salts_proxy_side *rearm_source;
} salts_proxy_side_t;

typedef struct salts_proxy_session {
  struct salts_tcp_proxy_s *proxy;
  size_t index;
  salts_proxy_phase_t phase;
  salts_proxy_protocol protocol;
  salts_proxy_side_t downstream;
  salts_proxy_side_t upstream;
  cnet_stream_peer peer;
  unsigned char *handshake;
  size_t handshake_size;
  char target_host[SALTS_PROXY_HOST_CAPACITY];
  uint16_t target_port;
  size_t upstream_index; /* SIZE_MAX means no static-policy reservation. */
} salts_proxy_session_t;

struct salts_tcp_proxy_s {
  salts_tcp_proxy_config_t config;
  cnet_client client;
  cnet_listener listener;
  salts_proxy_session_t *sessions;
  unsigned char *handshake_storage;
  char *raw_backend_uri;
  char *username;
  char *password;
  char *http_authorization;
  salts_tcp_proxy_upstream_t *upstreams; /* URI ownership retained to destroy. */
  cnet_destination_hint *upstream_hints;
  uint64_t next_upstream_sequence;
  int client_initialized;
  int listener_initialized;
  int fatal_status;
  int stopping;
  int stopped;
};

static native_io_backend_kind salts_proxy_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static int salts_proxy_is_power_of_two(size_t value) {
  return value != 0u && (value & (value - 1u)) == 0u;
}

static int salts_proxy_handle_equal(cnet_connection left, cnet_connection right) {
  return left.slot == right.slot && left.generation == right.generation;
}

static char *salts_proxy_copy_string(const char *value) {
  size_t size;
  char *copy;
  if (!value) return NULL;
  size = strlen(value) + 1u;
  copy = (char *)malloc(size);
  if (copy) memcpy(copy, value, size);
  return copy;
}

/* The physical stream is a dedicated TCP tunnel, so an upstream becomes
 * available only after a true CNet terminal, never after send completion. */
static void salts_proxy_release_upstream(salts_proxy_session_t *session) {
  size_t index = session->upstream_index;
  if (index == SIZE_MAX) return;
  session->upstream_index = SIZE_MAX;
  if (index < session->proxy->config.upstream_count &&
      session->proxy->upstream_hints[index].inflight != 0u) {
    --session->proxy->upstream_hints[index].inflight;
  } else {
    session->proxy->fatal_status = SALTS_EPROTO;
  }
}

static void salts_proxy_release_session(salts_proxy_session_t *session) {
  if (session->downstream.live || session->upstream.live) return;
  salts_proxy_release_upstream(session);
  session->phase = SALTS_PROXY_PHASE_FREE;
  session->protocol = SALTS_PROXY_PROTOCOL_AUTO;
  session->handshake_size = 0u;
  session->target_host[0] = '\0';
  session->target_port = 0u;
  memset(&session->peer, 0, sizeof(session->peer));
  memset(&session->downstream.connection, 0, sizeof(session->downstream.connection));
  memset(&session->upstream.connection, 0, sizeof(session->upstream.connection));
  session->downstream.connected = 0;
  session->upstream.connected = 0;
  session->downstream.receive_armed = 0;
  session->upstream.receive_armed = 0;
  session->downstream.send_action = SALTS_PROXY_SEND_NONE;
  session->upstream.send_action = SALTS_PROXY_SEND_NONE;
  session->downstream.rearm_source = NULL;
  session->upstream.rearm_source = NULL;
}

static void salts_proxy_close_side(salts_proxy_side_t *side) {
  int status;
  if (!side || !side->live) return;
  side->receive_armed = 0;
  status = cnet_close(&side->session->proxy->client, side->connection);
  if (status != SALTS_OK && status != SALTS_EALREADY && status != SALTS_ESHUTDOWN &&
      status != SALTS_ENOENT) {
    side->session->proxy->fatal_status = status;
  }
}

static void salts_proxy_close_session(salts_proxy_session_t *session) {
  if (!session || session->phase == SALTS_PROXY_PHASE_FREE) return;
  session->phase = SALTS_PROXY_PHASE_CLOSING;
  salts_proxy_close_side(&session->downstream);
  salts_proxy_close_side(&session->upstream);
}

static int salts_proxy_arm_receive(salts_proxy_side_t *side) {
  int status;
  if (!side || !side->live || !side->connected || side->receive_armed) return SALTS_OK;
  status = cnet_receive(&side->session->proxy->client, side->connection, 1u);
  if (status == SALTS_OK) side->receive_armed = 1;
  return status;
}

static void salts_proxy_consume_handshake(salts_proxy_session_t *session, size_t size) {
  if (size >= session->handshake_size) {
    session->handshake_size = 0u;
    return;
  }
  memmove(session->handshake, session->handshake + size, session->handshake_size - size);
  session->handshake_size -= size;
}

static int salts_proxy_ascii_equal(const char *left, size_t left_size, const char *right) {
  size_t index;
  size_t right_size = strlen(right);
  if (left_size != right_size) return 0;
  for (index = 0u; index < left_size; ++index) {
    unsigned char a = (unsigned char)left[index];
    unsigned char b = (unsigned char)right[index];
    if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
    if (a != b) return 0;
  }
  return 1;
}

static int salts_proxy_constant_time_equal(const char *left, size_t left_size, const char *right) {
  size_t index;
  size_t right_size = strlen(right);
  size_t difference = left_size ^ right_size;
  size_t common = left_size < right_size ? left_size : right_size;
  for (index = 0u; index < common; ++index) {
    difference |= (size_t)((unsigned char)left[index] ^ (unsigned char)right[index]);
  }
  return difference == 0u;
}

static size_t salts_proxy_find_header_end(const unsigned char *data, size_t size) {
  size_t index;
  if (size < 4u) return 0u;
  for (index = 0u; index + 3u < size; ++index) {
    if (data[index] == '\r' && data[index + 1u] == '\n' && data[index + 2u] == '\r' &&
        data[index + 3u] == '\n') {
      return index + 4u;
    }
  }
  return 0u;
}

static int salts_proxy_parse_port(const char *text, size_t size, uint16_t *out_port) {
  size_t index;
  unsigned long value = 0u;
  if (!text || size == 0u || !out_port) return 0;
  for (index = 0u; index < size; ++index) {
    if (text[index] < '0' || text[index] > '9') return 0;
    value = value * 10u + (unsigned long)(text[index] - '0');
    if (value > UINT16_MAX) return 0;
  }
  if (value == 0u) return 0;
  *out_port = (uint16_t)value;
  return 1;
}

static int salts_proxy_parse_authority(const char *authority, size_t size, char *out_host,
                                       uint16_t *out_port) {
  const char *host = authority;
  const char *port;
  size_t host_size;
  size_t index;
  if (!authority || !out_host || !out_port || size == 0u) return 0;
  if (authority[0] == '[') {
    for (index = 1u; index < size && authority[index] != ']'; ++index) {}
    if (index == size || index + 1u >= size || authority[index + 1u] != ':') return 0;
    host = authority + 1u;
    host_size = index - 1u;
    port = authority + index + 2u;
  } else {
    for (index = size; index > 0u && authority[index - 1u] != ':'; --index) {}
    if (index == 0u) return 0;
    host_size = index - 1u;
    port = authority + index;
    for (size_t scan = 0u; scan < host_size; ++scan) {
      if (authority[scan] == ':') return 0;
    }
  }
  if (host_size == 0u || host_size >= SALTS_PROXY_HOST_CAPACITY) return 0;
  if (!salts_proxy_parse_port(port, (size_t)(authority + size - port), out_port)) return 0;
  memcpy(out_host, host, host_size);
  out_host[host_size] = '\0';
  return 1;
}

static int salts_proxy_http_authorized(const salts_tcp_proxy_t *proxy, const unsigned char *data,
                                       size_t request_line_end, size_t header_end) {
  size_t cursor;
  if (!proxy->http_authorization) return 1;
  cursor = request_line_end + 2u;
  while (cursor + 2u <= header_end) {
    size_t line_end = cursor;
    size_t colon;
    size_t value_begin;
    size_t value_end;
    while (line_end + 1u < header_end &&
           !(data[line_end] == '\r' && data[line_end + 1u] == '\n')) {
      ++line_end;
    }
    if (line_end == cursor) break;
    for (colon = cursor; colon < line_end && data[colon] != ':'; ++colon) {}
    if (colon < line_end &&
        salts_proxy_ascii_equal((const char *)data + cursor, colon - cursor,
                                "Proxy-Authorization")) {
      value_begin = colon + 1u;
      while (value_begin < line_end &&
             (data[value_begin] == ' ' || data[value_begin] == '\t')) {
        ++value_begin;
      }
      value_end = line_end;
      while (value_end > value_begin &&
             (data[value_end - 1u] == ' ' || data[value_end - 1u] == '\t')) {
        --value_end;
      }
      if (salts_proxy_constant_time_equal((const char *)data + value_begin,
                                          value_end - value_begin,
                                          proxy->http_authorization)) {
        return 1;
      }
    }
    cursor = line_end + 2u;
  }
  return 0;
}

static void salts_proxy_process_handshake(salts_proxy_session_t *session);
static void salts_proxy_start_pump(salts_proxy_session_t *session);

static int salts_proxy_send_control(salts_proxy_session_t *session, const void *data, size_t size,
                                    salts_proxy_send_action_t action) {
  int status = salts_proxy_cnet_send_bytes(
      &session->proxy->client, session->downstream.connection, data, size, 0);
  if (status == SALTS_OK) {
    session->downstream.send_action = action;
  } else {
    salts_proxy_close_session(session);
  }
  return status;
}

static void salts_proxy_send_final(salts_proxy_session_t *session, const void *data, size_t size) {
  int status;
  if (!session->downstream.live || !session->downstream.connected) {
    salts_proxy_close_session(session);
    return;
  }
  session->phase = SALTS_PROXY_PHASE_CLOSING;
  status = salts_proxy_cnet_send_bytes(
      &session->proxy->client, session->downstream.connection, data, size, 1);
  if (status != SALTS_OK) salts_proxy_close_side(&session->downstream);
  salts_proxy_close_side(&session->upstream);
}

static void salts_proxy_fail_protocol(salts_proxy_session_t *session) {
  if (session->protocol == SALTS_PROXY_PROTOCOL_HTTP_CONNECT) {
    salts_proxy_send_final(session, salts_http_bad_request, sizeof(salts_http_bad_request) - 1u);
  } else if (session->protocol == SALTS_PROXY_PROTOCOL_SOCKS5) {
    unsigned char response[10] = {5u, 1u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
    salts_proxy_send_final(session, response, sizeof(response));
  } else {
    salts_proxy_close_session(session);
  }
}

static int salts_proxy_format_uri(const char *host, uint16_t port, char *uri, size_t capacity) {
  const char *format = strchr(host, ':') ? "tcp://[%s]:%u" : "tcp://%s:%u";
  int written = snprintf(uri, capacity, format, host, (unsigned int)port);
  return written > 0 && (size_t)written < capacity;
}

static void salts_proxy_forward(salts_proxy_side_t *source, const cnet_receive_view *view) {
  salts_proxy_session_t *session = source->session;
  salts_proxy_side_t *destination = source->role == SALTS_PROXY_SIDE_DOWNSTREAM
                                        ? &session->upstream
                                        : &session->downstream;
  int status;
  if (session->phase != SALTS_PROXY_PHASE_PUMP || !destination->live ||
      !destination->connected) {
    salts_proxy_close_session(session);
    return;
  }
  status = salts_proxy_cnet_send_bytes(
      &session->proxy->client, destination->connection, view->data, view->size, 0);
  if (status != SALTS_OK) {
    salts_proxy_close_session(session);
    return;
  }
  destination->send_action = SALTS_PROXY_SEND_REARM_SOURCE;
  destination->rearm_source = source;
}

static void salts_proxy_start_pump(salts_proxy_session_t *session) {
  int status;
  session->phase = SALTS_PROXY_PHASE_PUMP;
  status = salts_proxy_arm_receive(&session->upstream);
  if (status != SALTS_OK) {
    salts_proxy_close_session(session);
    return;
  }
  if (session->handshake_size != 0u) {
    status = salts_proxy_cnet_send_bytes(
        &session->proxy->client, session->upstream.connection,
        session->handshake, session->handshake_size, 0);
    if (status != SALTS_OK) {
      salts_proxy_close_session(session);
      return;
    }
    session->upstream.send_action = SALTS_PROXY_SEND_REARM_SOURCE;
    session->upstream.rearm_source = &session->downstream;
    session->handshake_size = 0u;
  } else if (salts_proxy_arm_receive(&session->downstream) != SALTS_OK) {
    salts_proxy_close_session(session);
  }
}

static void salts_proxy_on_state(void *user, cnet_connection connection,
                                 cnet_connection_state state, const cnet_error *error);
static void salts_proxy_on_receive(void *user, cnet_connection connection,
                                   const cnet_receive_view *view);
static void salts_proxy_on_send(void *user, cnet_connection connection, size_t size);

static cnet_observer salts_proxy_observer(salts_proxy_side_t *side) {
  return (cnet_observer){salts_proxy_on_state, salts_proxy_on_receive, side, salts_proxy_on_send};
}

static void salts_proxy_upstream_failed(salts_proxy_session_t *session);

/* Evaluate the immutable, fully authorized CNet endpoint snapshot on the
 * owner. Health/inflight observations are advisory, but the session reservation
 * is real and terminal-settled. No silent retry/fallback or cross-owner lease. */
static int salts_proxy_choose_upstream(salts_proxy_session_t *session, const char **out_uri) {
  salts_tcp_proxy_t *proxy = session->proxy;
  cnet_destination_selection selection;
  cnet_destination_result result;
  int status;
  if (!out_uri || !proxy->config.upstream_count) return SALTS_EINVAL;
  selection = (cnet_destination_selection){
      .size = sizeof(selection),
      .version = CNET_DESTINATION_POLICY_VERSION,
      .kind = proxy->config.upstream_policy,
      .endpoints = proxy->upstream_hints,
      .endpoint_count = proxy->config.upstream_count,
      .snapshot_generation = 1u,
      .expires_at_ms = UINT64_MAX,
      .sequence = proxy->next_upstream_sequence,
      .explicit_endpoint_id = proxy->config.explicit_upstream_id};
  status = cnet_destination_choose(&selection, &result);
  if (status != SALTS_OK) return status;
  if (result.index >= proxy->config.upstream_count ||
      proxy->upstream_hints[result.index].inflight == UINT64_MAX) {
    return SALTS_ENOBUFS;
  }
  ++proxy->next_upstream_sequence;
  ++proxy->upstream_hints[result.index].inflight;
  session->upstream_index = result.index;
  *out_uri = proxy->upstreams[result.index].uri;
  return SALTS_OK;
}

static void salts_proxy_connect_upstream_ready(salts_proxy_session_t *session) {
  salts_tcp_proxy_t *proxy = session->proxy;
  salts_tcp_proxy_route_request_t request;
  cnet_connect_options options;
  cnet_observer observer;
  const char *uri;
  char direct_uri[SALTS_PROXY_URI_CAPACITY];
  int status;

  request = (salts_tcp_proxy_route_request_t){session->protocol,
                                               session->target_host[0] ? session->target_host : NULL,
                                               session->target_port, &session->peer};
  if (proxy->config.upstream_count != 0u) {
    status = salts_proxy_choose_upstream(session, &uri);
    if (status != SALTS_OK) {
      salts_proxy_upstream_failed(session);
      return;
    }
  } else {
    uri = proxy->config.route ? proxy->config.route(&request, proxy->config.route_user) : NULL;
    if (!uri) {
      if (session->protocol == SALTS_PROXY_PROTOCOL_RAW) {
        uri = proxy->raw_backend_uri;
      } else if (!salts_proxy_format_uri(session->target_host, session->target_port, direct_uri,
                                         sizeof(direct_uri))) {
        salts_proxy_fail_protocol(session);
        return;
      } else {
        uri = direct_uri;
      }
    }
  }
  if (!uri || !uri[0]) {
    salts_proxy_release_upstream(session);
    salts_proxy_fail_protocol(session);
    return;
  }
  observer = salts_proxy_observer(&session->upstream);
  options = (cnet_connect_options){.uri = uri, .observer = observer};
  session->phase = SALTS_PROXY_PHASE_CONNECTING;
  status = cnet_connect(&proxy->client, &options, &session->upstream.connection);
  if (status != SALTS_OK) {
    salts_proxy_release_upstream(session);
    salts_proxy_upstream_failed(session);
    return;
  }
  session->upstream.live = 1;
}

static void salts_proxy_process_http(salts_proxy_session_t *session) {
  salts_tcp_proxy_t *proxy = session->proxy;
  const unsigned char *data = session->handshake;
  size_t size = session->handshake_size;
  size_t header_end = salts_proxy_find_header_end(data, size);
  size_t request_line_end = 0u;
  size_t authority_begin;
  size_t authority_end;
  if (!header_end) {
    if (size == proxy->config.handshake_capacity ||
        salts_proxy_arm_receive(&session->downstream) != SALTS_OK) {
      salts_proxy_send_final(session, salts_http_bad_request,
                             sizeof(salts_http_bad_request) - 1u);
    }
    return;
  }
  while (request_line_end + 1u < header_end &&
         !(data[request_line_end] == '\r' && data[request_line_end + 1u] == '\n')) {
    ++request_line_end;
  }
  if (request_line_end + 1u >= header_end || request_line_end < 16u ||
      memcmp(data, "CONNECT ", 8u) != 0) {
    salts_proxy_send_final(session, salts_http_bad_request, sizeof(salts_http_bad_request) - 1u);
    return;
  }
  authority_begin = 8u;
  for (authority_end = authority_begin;
       authority_end < request_line_end && data[authority_end] != ' '; ++authority_end) {}
  if (authority_end == authority_begin || authority_end == request_line_end ||
      (!salts_proxy_ascii_equal((const char *)data + authority_end + 1u,
                                request_line_end - authority_end - 1u, "HTTP/1.1") &&
       !salts_proxy_ascii_equal((const char *)data + authority_end + 1u,
                                request_line_end - authority_end - 1u, "HTTP/1.0")) ||
      !salts_proxy_parse_authority((const char *)data + authority_begin,
                                   authority_end - authority_begin, session->target_host,
                                   &session->target_port)) {
    salts_proxy_send_final(session, salts_http_bad_request, sizeof(salts_http_bad_request) - 1u);
    return;
  }
  if (!salts_proxy_http_authorized(proxy, data, request_line_end, header_end)) {
    salts_proxy_send_final(session, salts_http_auth_required,
                           sizeof(salts_http_auth_required) - 1u);
    return;
  }
  salts_proxy_consume_handshake(session, header_end);
  salts_proxy_connect_upstream_ready(session);
}

static int salts_proxy_socks_method_available(const unsigned char *methods, size_t count,
                                              unsigned char expected) {
  size_t index;
  for (index = 0u; index < count; ++index) {
    if (methods[index] == expected) return 1;
  }
  return 0;
}

static void salts_proxy_process_socks(salts_proxy_session_t *session) {
  salts_tcp_proxy_t *proxy = session->proxy;
  const unsigned char *data = session->handshake;
  size_t size = session->handshake_size;
  size_t required;
  unsigned char response[2];

  if (session->phase == SALTS_PROXY_PHASE_SOCKS_GREETING) {
    if (size < 2u) goto need_more;
    required = 2u + data[1];
    if (data[0] != 5u || data[1] == 0u) goto malformed;
    if (size < required) goto need_more;
    response[0] = 5u;
    response[1] = proxy->username ? 2u : 0u;
    if (!salts_proxy_socks_method_available(data + 2u, data[1], response[1])) {
      response[1] = 255u;
      salts_proxy_send_final(session, response, sizeof(response));
      return;
    }
    salts_proxy_consume_handshake(session, required);
    session->phase = proxy->username ? SALTS_PROXY_PHASE_SOCKS_AUTH
                                     : SALTS_PROXY_PHASE_SOCKS_REQUEST;
    (void)salts_proxy_send_control(session, response, sizeof(response),
                                   SALTS_PROXY_SEND_PROCESS_HANDSHAKE);
    return;
  }

  if (session->phase == SALTS_PROXY_PHASE_SOCKS_AUTH) {
    size_t username_size;
    size_t password_size;
    int accepted;
    if (size < 2u) goto need_more;
    username_size = data[1];
    if (data[0] != 1u || username_size == 0u || size < 3u + username_size) goto malformed;
    password_size = data[2u + username_size];
    required = 3u + username_size + password_size;
    if (password_size == 0u) goto malformed;
    if (size < required) goto need_more;
    accepted = salts_proxy_constant_time_equal((const char *)data + 2u, username_size,
                                                proxy->username) &&
               salts_proxy_constant_time_equal((const char *)data + 3u + username_size,
                                                password_size, proxy->password);
    response[0] = 1u;
    response[1] = accepted ? 0u : 1u;
    salts_proxy_consume_handshake(session, required);
    if (!accepted) {
      salts_proxy_send_final(session, response, sizeof(response));
      return;
    }
    session->phase = SALTS_PROXY_PHASE_SOCKS_REQUEST;
    (void)salts_proxy_send_control(session, response, sizeof(response),
                                   SALTS_PROXY_SEND_PROCESS_HANDSHAKE);
    return;
  }

  if (session->phase == SALTS_PROXY_PHASE_SOCKS_REQUEST) {
    size_t address_size;
    size_t address_offset = 4u;
    size_t port_offset;
    if (size < 4u) goto need_more;
    if (data[0] != 5u || data[2] != 0u) goto malformed;
    if (data[1] != 1u) {
      unsigned char unsupported[10] = {5u, 7u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
      salts_proxy_send_final(session, unsupported, sizeof(unsupported));
      return;
    }
    if (data[3] == 1u) {
      address_size = 4u;
      required = 4u + address_size + 2u;
      if (size < required) goto need_more;
      if (snprintf(session->target_host, sizeof(session->target_host), "%u.%u.%u.%u",
                   (unsigned int)data[4], (unsigned int)data[5], (unsigned int)data[6],
                   (unsigned int)data[7]) <= 0) goto malformed;
    } else if (data[3] == 3u) {
      if (size < 5u) goto need_more;
      address_size = data[4];
      address_offset = 5u;
      required = address_offset + address_size + 2u;
      if (address_size == 0u || address_size >= sizeof(session->target_host)) goto malformed;
      if (size < required) goto need_more;
      memcpy(session->target_host, data + address_offset, address_size);
      session->target_host[address_size] = '\0';
    } else if (data[3] == 4u) {
      address_size = 16u;
      required = 4u + address_size + 2u;
      if (size < required) goto need_more;
      if (snprintf(session->target_host, sizeof(session->target_host),
                   "%x:%x:%x:%x:%x:%x:%x:%x", (data[4] << 8) | data[5],
                   (data[6] << 8) | data[7], (data[8] << 8) | data[9],
                   (data[10] << 8) | data[11], (data[12] << 8) | data[13],
                   (data[14] << 8) | data[15], (data[16] << 8) | data[17],
                   (data[18] << 8) | data[19]) <= 0) goto malformed;
    } else {
      unsigned char unsupported[10] = {5u, 8u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
      salts_proxy_send_final(session, unsupported, sizeof(unsupported));
      return;
    }
    port_offset = address_offset + address_size;
    session->target_port = (uint16_t)(((uint16_t)data[port_offset] << 8u) |
                                      (uint16_t)data[port_offset + 1u]);
    if (session->target_port == 0u) goto malformed;
    salts_proxy_consume_handshake(session, required);
    salts_proxy_connect_upstream_ready(session);
    return;
  }
  return;

need_more:
  if (size == proxy->config.handshake_capacity ||
      salts_proxy_arm_receive(&session->downstream) != SALTS_OK) {
    salts_proxy_fail_protocol(session);
  }
  return;
malformed:
  salts_proxy_fail_protocol(session);
}

static void salts_proxy_process_handshake(salts_proxy_session_t *session) {
  salts_tcp_proxy_t *proxy = session->proxy;
  if (session->phase == SALTS_PROXY_PHASE_AUTO) {
    static const char connect_prefix[] = "CONNECT ";
    size_t compare_size;
    if (session->handshake_size == 0u) {
      if (salts_proxy_arm_receive(&session->downstream) != SALTS_OK)
        salts_proxy_close_session(session);
      return;
    }
    if (session->handshake[0] == 5u) {
      session->protocol = SALTS_PROXY_PROTOCOL_SOCKS5;
      session->phase = SALTS_PROXY_PHASE_SOCKS_GREETING;
    } else {
      compare_size = session->handshake_size < sizeof(connect_prefix) - 1u
                         ? session->handshake_size
                         : sizeof(connect_prefix) - 1u;
      if (memcmp(session->handshake, connect_prefix, compare_size) != 0) {
        salts_proxy_close_session(session);
        return;
      }
      if (session->handshake_size < sizeof(connect_prefix) - 1u) {
        if (salts_proxy_arm_receive(&session->downstream) != SALTS_OK)
          salts_proxy_close_session(session);
        return;
      }
      session->protocol = SALTS_PROXY_PROTOCOL_HTTP_CONNECT;
      session->phase = SALTS_PROXY_PHASE_HTTP;
    }
  }
  if (session->phase == SALTS_PROXY_PHASE_HTTP) {
    salts_proxy_process_http(session);
  } else {
    salts_proxy_process_socks(session);
  }
  (void)proxy;
}

static void salts_proxy_upstream_failed(salts_proxy_session_t *session) {
  if (!session->downstream.live || !session->downstream.connected) return;
  if (session->protocol == SALTS_PROXY_PROTOCOL_HTTP_CONNECT) {
    salts_proxy_send_final(session, salts_http_bad_gateway, sizeof(salts_http_bad_gateway) - 1u);
  } else if (session->protocol == SALTS_PROXY_PROTOCOL_SOCKS5) {
    unsigned char response[10] = {5u, 5u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u};
    salts_proxy_send_final(session, response, sizeof(response));
  } else {
    salts_proxy_close_side(&session->downstream);
  }
}

static void salts_proxy_on_state(void *user, cnet_connection connection,
                                 cnet_connection_state state, const cnet_error *error) {
  salts_proxy_side_t *side = (salts_proxy_side_t *)user;
  salts_proxy_session_t *session;
  (void)error;
  if (!side || !salts_proxy_handle_equal(side->connection, connection)) return;
  session = side->session;
  if (state == CNET_CONNECTION_CONNECTED) {
    side->connected = 1;
    if (side->role == SALTS_PROXY_SIDE_UPSTREAM) {
      if (session->phase != SALTS_PROXY_PHASE_CONNECTING) {
        salts_proxy_close_session(session);
      } else if (session->protocol == SALTS_PROXY_PROTOCOL_RAW) {
        salts_proxy_start_pump(session);
      } else {
        const void *reply = session->protocol == SALTS_PROXY_PROTOCOL_HTTP_CONNECT
                                ? (const void *)salts_http_success
                                : (const void *)salts_socks_success;
        size_t reply_size = session->protocol == SALTS_PROXY_PROTOCOL_HTTP_CONNECT
                                ? sizeof(salts_http_success) - 1u
                                : sizeof(salts_socks_success);
        session->phase = SALTS_PROXY_PHASE_REPLYING;
        (void)salts_proxy_send_control(session, reply, reply_size, SALTS_PROXY_SEND_START_PUMP);
      }
    } else if (session->phase == SALTS_PROXY_PHASE_DENIED) {
      salts_proxy_close_session(session);
    } else if (session->protocol == SALTS_PROXY_PROTOCOL_RAW) {
      salts_proxy_connect_upstream_ready(session);
    } else {
      session->phase = session->protocol == SALTS_PROXY_PROTOCOL_AUTO
                           ? SALTS_PROXY_PHASE_AUTO
                           : session->protocol == SALTS_PROXY_PROTOCOL_HTTP_CONNECT
                                 ? SALTS_PROXY_PHASE_HTTP
                                 : SALTS_PROXY_PHASE_SOCKS_GREETING;
      salts_proxy_process_handshake(session);
    }
    return;
  }
  if (state != CNET_CONNECTION_CLOSED && state != CNET_CONNECTION_FAILED) return;
  side->live = 0;
  if (side->role == SALTS_PROXY_SIDE_UPSTREAM) salts_proxy_release_upstream(session);
  side->connected = 0;
  side->receive_armed = 0;
  side->send_action = SALTS_PROXY_SEND_NONE;
  side->rearm_source = NULL;
  if (side->role == SALTS_PROXY_SIDE_UPSTREAM && state == CNET_CONNECTION_FAILED &&
      session->phase == SALTS_PROXY_PHASE_CONNECTING) {
    salts_proxy_upstream_failed(session);
  } else {
    salts_proxy_close_side(side->role == SALTS_PROXY_SIDE_DOWNSTREAM ? &session->upstream
                                                                     : &session->downstream);
  }
  salts_proxy_release_session(session);
}

static void salts_proxy_on_receive(void *user, cnet_connection connection,
                                   const cnet_receive_view *view) {
  salts_proxy_side_t *side = (salts_proxy_side_t *)user;
  salts_proxy_session_t *session;
  if (!side || !salts_proxy_handle_equal(side->connection, connection)) return;
  side->receive_armed = 0;
  session = side->session;
  if (!view || view->kind != CNET_MESSAGE_BYTES || !view->data || view->size == 0u ||
      view->size > session->proxy->config.max_message_bytes) {
    salts_proxy_close_session(session);
    return;
  }
  if (session->phase == SALTS_PROXY_PHASE_PUMP) {
    salts_proxy_forward(side, view);
    return;
  }
  if (side->role != SALTS_PROXY_SIDE_DOWNSTREAM ||
      view->size > session->proxy->config.handshake_capacity - session->handshake_size) {
    salts_proxy_fail_protocol(session);
    return;
  }
  memcpy(session->handshake + session->handshake_size, view->data, view->size);
  session->handshake_size += view->size;
  salts_proxy_process_handshake(session);
}

static void salts_proxy_on_send(void *user, cnet_connection connection, size_t size) {
  salts_proxy_side_t *side = (salts_proxy_side_t *)user;
  salts_proxy_send_action_t action;
  salts_proxy_side_t *source;
  (void)size;
  if (!side || !salts_proxy_handle_equal(side->connection, connection)) return;
  action = side->send_action;
  source = side->rearm_source;
  side->send_action = SALTS_PROXY_SEND_NONE;
  side->rearm_source = NULL;
  if (action == SALTS_PROXY_SEND_PROCESS_HANDSHAKE) {
    salts_proxy_process_handshake(side->session);
  } else if (action == SALTS_PROXY_SEND_START_PUMP) {
    salts_proxy_start_pump(side->session);
  } else if (action == SALTS_PROXY_SEND_REARM_SOURCE && source &&
             salts_proxy_arm_receive(source) != SALTS_OK) {
    salts_proxy_close_session(side->session);
  }
}

salts_tcp_proxy_config_t salts_tcp_proxy_config_default(void) {
  salts_tcp_proxy_config_t config;
  memset(&config, 0, sizeof(config));
  config.protocol = SALTS_PROXY_PROTOCOL_AUTO;
  config.session_capacity = SALTS_PROXY_DEFAULT_SESSION_CAPACITY;
  config.handshake_capacity = SALTS_PROXY_DEFAULT_HANDSHAKE_CAPACITY;
  config.max_message_bytes = SALTS_PROXY_DEFAULT_MAX_MESSAGE_BYTES;
  config.command_capacity = SALTS_PROXY_DEFAULT_COMMAND_CAPACITY;
  config.request_capacity = SALTS_PROXY_DEFAULT_REQUEST_CAPACITY;
  config.event_capacity = SALTS_PROXY_DEFAULT_EVENT_CAPACITY;
  config.completion_batch_capacity = SALTS_PROXY_DEFAULT_COMPLETION_CAPACITY;
  config.backlog = SALTS_PROXY_DEFAULT_BACKLOG;
  config.connect_timeout_ms = SALTS_PROXY_DEFAULT_IO_TIMEOUT_MS;
  config.read_timeout_ms = SALTS_PROXY_DEFAULT_IO_TIMEOUT_MS;
  config.write_timeout_ms = SALTS_PROXY_DEFAULT_IO_TIMEOUT_MS;
  config.shutdown_timeout_ms = SALTS_PROXY_DEFAULT_SHUTDOWN_TIMEOUT_MS;
  config.upstream_policy = CNET_DESTINATION_ROUND_ROBIN;
  return config;
}

/* Configuration is copied once. Do not admit request-dependent URI escapes
 * around the static endpoint eligibility and caller authorization boundary. */
static int salts_proxy_upstreams_valid(const salts_tcp_proxy_config_t *config) {
  size_t i;
  uint64_t previous = 0u;
  int explicit_found = 0;
  if (config->upstream_policy != CNET_DESTINATION_ROUND_ROBIN &&
      config->upstream_policy != CNET_DESTINATION_WEIGHTED_RR &&
      config->upstream_policy != CNET_DESTINATION_LEAST_INFLIGHT &&
      config->upstream_policy != CNET_DESTINATION_EXPLICIT) return 0;
  if (config->upstream_count == 0u)
    return config->upstreams == NULL && config->explicit_upstream_id == 0u;
  if (!config->upstreams || config->route || config->raw_backend_uri ||
      config->upstream_count > 1024u ||
      config->upstream_count > SIZE_MAX / sizeof(salts_tcp_proxy_upstream_t) ||
      config->upstream_count > SIZE_MAX / sizeof(cnet_destination_hint)) return 0;
  for (i = 0u; i < config->upstream_count; ++i) {
    const salts_tcp_proxy_upstream_t *up = &config->upstreams[i];
    if (up->endpoint_id <= previous || up->weight == 0u ||
        up->uri == NULL || strncmp(up->uri, "tcp://", 6u) != 0 || up->uri[6] == '\0') return 0;
    previous = up->endpoint_id;
    if (up->endpoint_id == config->explicit_upstream_id) explicit_found = 1;
  }
  return config->upstream_policy != CNET_DESTINATION_EXPLICIT ||
         (config->explicit_upstream_id != 0u && explicit_found);
}

static int salts_proxy_config_valid(const salts_tcp_proxy_config_t *config) {
  int protocol_valid;
  if (!config) return 0;
  protocol_valid = config->protocol == SALTS_PROXY_PROTOCOL_AUTO ||
                   config->protocol == SALTS_PROXY_PROTOCOL_SOCKS5 ||
                   config->protocol == SALTS_PROXY_PROTOCOL_HTTP_CONNECT ||
                   config->protocol == SALTS_PROXY_PROTOCOL_RAW;
  return protocol_valid && config->session_capacity != 0u &&
         config->session_capacity <= SIZE_MAX / 2u && config->handshake_capacity != 0u &&
         config->max_message_bytes >= sizeof(salts_http_auth_required) - 1u &&
         config->handshake_capacity <= SIZE_MAX / config->session_capacity &&
         salts_proxy_is_power_of_two(config->command_capacity) &&
         config->request_capacity != 0u && salts_proxy_is_power_of_two(config->event_capacity) &&
         config->completion_batch_capacity != 0u &&
         config->completion_batch_capacity <= config->request_capacity && config->backlog != 0u &&
         config->shutdown_timeout_ms != 0u &&
         ((config->username == NULL) == (config->password == NULL)) &&
         (!config->username || (config->protocol != SALTS_PROXY_PROTOCOL_RAW &&
                                strlen(config->username) > 0u && strlen(config->username) <= 255u &&
                                strlen(config->password) > 0u && strlen(config->password) <= 255u)) &&
         (config->protocol != SALTS_PROXY_PROTOCOL_RAW ||
          ((config->raw_backend_uri && config->raw_backend_uri[0]) ||
            config->upstream_count != 0u)) &&
          salts_proxy_upstreams_valid(config);
}

static int salts_proxy_prepare_auth(salts_tcp_proxy_t *proxy) {
  size_t username_size;
  size_t password_size;
  size_t plain_size;
  size_t encoded_size;
  unsigned char plain[511];
  if (!proxy->config.username) return 1;
  proxy->username = salts_proxy_copy_string(proxy->config.username);
  proxy->password = salts_proxy_copy_string(proxy->config.password);
  if (!proxy->username || !proxy->password) return 0;
  username_size = strlen(proxy->username);
  password_size = strlen(proxy->password);
  plain_size = username_size + 1u + password_size;
  memcpy(plain, proxy->username, username_size);
  plain[username_size] = ':';
  memcpy(plain + username_size + 1u, proxy->password, password_size);
  encoded_size = 4u * ((plain_size + 2u) / 3u);
  if (encoded_size > SIZE_MAX - 7u) return 0;
  proxy->http_authorization = (char *)malloc(6u + encoded_size + 1u);
  if (!proxy->http_authorization) return 0;
  memcpy(proxy->http_authorization, "Basic ", 6u);
  return tn_base64_encode_buf_ex(plain, plain_size, proxy->http_authorization + 6u,
                                 encoded_size + 1u) == TN_BASE64_OK;
}

salts_tcp_proxy_t *salts_tcp_proxy_create(const salts_tcp_proxy_config_t *config) {
  salts_tcp_proxy_t *proxy;
  cnet_client_config client_config;
  size_t index;
  if (!salts_proxy_config_valid(config)) return NULL;
  proxy = (salts_tcp_proxy_t *)calloc(1u, sizeof(*proxy));
  if (!proxy) return NULL;
  proxy->config = *config;
  if (config->upstream_count != 0u) {
    proxy->upstreams = (salts_tcp_proxy_upstream_t *)calloc(
        config->upstream_count, sizeof(*proxy->upstreams));
    proxy->upstream_hints = (cnet_destination_hint *)calloc(
        config->upstream_count, sizeof(*proxy->upstream_hints));
    if (!proxy->upstreams || !proxy->upstream_hints) goto fail;
    for (index = 0u; index < config->upstream_count; ++index) {
      const salts_tcp_proxy_upstream_t *endpoint = &config->upstreams[index];
      proxy->upstreams[index] = *endpoint;
      proxy->upstreams[index].uri = salts_proxy_copy_string(endpoint->uri);
      if (!proxy->upstreams[index].uri) goto fail;
      proxy->upstream_hints[index] = (cnet_destination_hint){
          .endpoint_id = endpoint->endpoint_id,
          .weight = endpoint->weight,
          .eligible = endpoint->eligible};
    }
    proxy->config.upstreams = proxy->upstreams;
  }
  if (config->raw_backend_uri) {
    proxy->raw_backend_uri = salts_proxy_copy_string(config->raw_backend_uri);
    if (!proxy->raw_backend_uri) goto fail;
    proxy->config.raw_backend_uri = proxy->raw_backend_uri;
  }
  if (!salts_proxy_prepare_auth(proxy)) goto fail;
  proxy->config.username = proxy->username;
  proxy->config.password = proxy->password;
  proxy->sessions =
      (salts_proxy_session_t *)calloc(config->session_capacity, sizeof(*proxy->sessions));
  proxy->handshake_storage =
      (unsigned char *)malloc(config->session_capacity * config->handshake_capacity);
  if (!proxy->sessions || !proxy->handshake_storage) goto fail;
  for (index = 0u; index < config->session_capacity; ++index) {
    salts_proxy_session_t *session = &proxy->sessions[index];
    session->proxy = proxy;
    session->index = index;
    session->phase = SALTS_PROXY_PHASE_FREE;
    session->upstream_index = SIZE_MAX;
    session->handshake = proxy->handshake_storage + index * config->handshake_capacity;
    session->downstream.session = session;
    session->downstream.role = SALTS_PROXY_SIDE_DOWNSTREAM;
    session->upstream.session = session;
    session->upstream.role = SALTS_PROXY_SIDE_UPSTREAM;
  }
  memset(&client_config, 0, sizeof(client_config));
  client_config.backend = salts_proxy_backend();
  client_config.connection_capacity = config->session_capacity * 2u;
  client_config.command_capacity = config->command_capacity;
  client_config.request_capacity = config->request_capacity;
  client_config.completion_batch_capacity = config->completion_batch_capacity;
  client_config.event_capacity = config->event_capacity;
  client_config.max_send_bytes = config->max_message_bytes;
  client_config.receive_buffer_bytes = config->max_message_bytes;
  client_config.connect_timeout_ms = config->connect_timeout_ms;
  client_config.read_timeout_ms = config->read_timeout_ms;
  client_config.write_timeout_ms = config->write_timeout_ms;
  if (cnet_client_init(&proxy->client, &client_config) != SALTS_OK) goto fail;
  proxy->client_initialized = 1;
  return proxy;

fail:
  if (proxy->upstreams) {
    for (index = 0u; index < config->upstream_count; ++index)
      free((void *)proxy->upstreams[index].uri);
  }
  free(proxy->upstreams);
  free(proxy->upstream_hints);
  free(proxy->http_authorization);
  free(proxy->password);
  free(proxy->username);
  free(proxy->raw_backend_uri);
  free(proxy->handshake_storage);
  free(proxy->sessions);
  free(proxy);
  return NULL;
}

int salts_tcp_proxy_listen(salts_tcp_proxy_t *proxy, const char *host, uint16_t port) {
  cnet_listener_config config;
  int status;
  if (!proxy || !host || !host[0] || proxy->listener_initialized || proxy->stopping) {
    return SALTS_EINVAL;
  }
  config = (cnet_listener_config){salts_proxy_backend(), host, port, proxy->config.backlog};
  status = cnet_listener_init(&proxy->listener, &config);
  if (status == SALTS_OK) proxy->listener_initialized = 1;
  return status;
}

int salts_tcp_proxy_port(const salts_tcp_proxy_t *proxy, uint16_t *out_port) {
  if (!proxy || !out_port || !proxy->listener_initialized) return SALTS_EINVAL;
  return cnet_listener_port(&proxy->listener, out_port);
}

static salts_proxy_session_t *salts_proxy_free_session(salts_tcp_proxy_t *proxy) {
  size_t index;
  for (index = 0u; index < proxy->config.session_capacity; ++index) {
    if (proxy->sessions[index].phase == SALTS_PROXY_PHASE_FREE) return &proxy->sessions[index];
  }
  return NULL;
}

static int salts_proxy_accept_ready(salts_tcp_proxy_t *proxy, size_t *accepted) {
  int ready;
  int status;
  for (;;) {
    salts_proxy_session_t *session;
    cnet_observer observer;
    status = cnet_listener_wait(&proxy->listener, 0u, &ready);
    if (status != SALTS_OK || !ready) return status;
    session = salts_proxy_free_session(proxy);
    if (!session) return SALTS_ENOBUFS;
    session->phase = SALTS_PROXY_PHASE_ACCEPTED;
    session->protocol = proxy->config.protocol;
    session->handshake_size = 0u;
    session->target_host[0] = '\0';
    session->target_port = 0u;
    session->upstream_index = SIZE_MAX;
    session->downstream.live = 1;
    session->downstream.connected = 0;
    session->downstream.receive_armed = 0;
    session->downstream.send_action = SALTS_PROXY_SEND_NONE;
    session->upstream.live = 0;
    session->upstream.connected = 0;
    session->upstream.receive_armed = 0;
    session->upstream.send_action = SALTS_PROXY_SEND_NONE;
    observer = salts_proxy_observer(&session->downstream);
    status = cnet_listener_accept_peer(&proxy->listener, &proxy->client, &observer,
                                       &session->downstream.connection, &session->peer);
    if (status != SALTS_OK) {
      session->downstream.live = 0;
      salts_proxy_release_session(session);
      return status == SALTS_ETIMEDOUT ? SALTS_OK : status;
    }
    if (proxy->config.access && !proxy->config.access(&session->peer, proxy->config.access_user)) {
      session->phase = SALTS_PROXY_PHASE_DENIED;
    }
    ++*accepted;
  }
}

int salts_tcp_proxy_poll(salts_tcp_proxy_t *proxy, uint32_t timeout_ms, size_t *out_events) {
  size_t accepted = 0u;
  size_t events = 0u;
  int status;
  if (!proxy || !out_events || !proxy->client_initialized || proxy->stopping) return SALTS_EINVAL;
  if (proxy->listener_initialized) {
    status = salts_proxy_accept_ready(proxy, &accepted);
    if (status != SALTS_OK && status != SALTS_ENOBUFS) return status;
  }
  status = cnet_client_poll(&proxy->client, timeout_ms, &events);
  if (status != SALTS_OK) return status;
  if (proxy->fatal_status != SALTS_OK) return proxy->fatal_status;
  *out_events = accepted + events;
  return SALTS_OK;
}

int salts_tcp_proxy_stop(salts_tcp_proxy_t *proxy) {
  int status;
  if (!proxy) return SALTS_EINVAL;
  if (proxy->stopped) return SALTS_OK;
  proxy->stopping = 1;
  if (proxy->listener_initialized) {
    status = cnet_listener_close(&proxy->listener);
    if (status != SALTS_OK && status != SALTS_EALREADY) return status;
    status = cnet_listener_destroy(&proxy->listener);
    if (status != SALTS_OK) return status;
    proxy->listener_initialized = 0;
  }
  status = cnet_client_stop(&proxy->client, proxy->config.shutdown_timeout_ms);
  if (status != SALTS_OK) return status;
  proxy->stopped = 1;
  return SALTS_OK;
}

int salts_tcp_proxy_destroy(salts_tcp_proxy_t *proxy) {
  int status;
  if (!proxy) return SALTS_EINVAL;
  if (!proxy->stopped) return SALTS_EBUSY;
  status = cnet_client_destroy(&proxy->client);
  if (status != SALTS_OK) return status;
  if (proxy->upstreams) {
    size_t index;
    for (index = 0u; index < proxy->config.upstream_count; ++index)
      free((void *)proxy->upstreams[index].uri);
  }
  free(proxy->upstreams);
  free(proxy->upstream_hints);
  free(proxy->http_authorization);
  free(proxy->password);
  free(proxy->username);
  free(proxy->raw_backend_uri);
  free(proxy->handshake_storage);
  free(proxy->sessions);
  free(proxy);
  return SALTS_OK;
}
