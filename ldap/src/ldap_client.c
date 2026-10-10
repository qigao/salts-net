/**
 * @file ldap_client.c
 * @brief LDAP Client implementation on top of caller-driven CNet streams.
 */

#include "ldap_client.h"
#include "ldap_builder.h"
#include "ldap_parser.h"
#include <cnet/cnet.h>
#include <cnet/manager.h>
#include <salts/clock.h>
#include <salts/error_codes.h>
#include <cmeta_buffer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Client error codes (match ldap_client.h) */
static int ldap_cnet_send_bytes(cnet_client *client,
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

#define LDAP_CLIENT_OK 0
#define LDAP_CLIENT_ERROR_MEMORY -1
#define LDAP_CLIENT_ERROR_INVALID -2
#define LDAP_CLIENT_ERROR_NETWORK -3
#define LDAP_CLIENT_ERROR_TIMEOUT -4
#define LDAP_CLIENT_ERROR_PROTOCOL -5
#define LDAP_CLIENT_ERROR_AUTH -6
#define LDAP_CLIENT_ERROR_TLS -7

enum {
  LDAP_DEFAULT_PORT = 389,
  LDAP_DEFAULT_TLS_PORT = 636,
  LDAP_DEFAULT_TIMEOUT_MS = 30000,
  LDAP_RECV_BUFFER_SIZE = 65536,
  LDAP_SEND_BUFFER_SIZE = 4096,
  LDAP_CNET_QUEUE_CAPACITY = 4,
  LDAP_CNET_URI_CAPACITY = 320,
  LDAP_CNET_STOP_TIMEOUT_MS = 5000
};

struct ldap_client_s {
  char *host;
  uint16_t port;
  uint32_t timeout_ms;
  int use_tls;

  int32_t next_message_id;
  char error_msg[256];
  int last_result_code;

  /* The synchronous caller is the sole CNet poll owner. */
  cnet_client net;
  cnet_manager manager;
  cnet_managed_connection managed;
  cnet_connection connection;
  int net_initialized;
  int connected;
  int terminal;
  int terminal_failed;
  int transport_status;
  int receive_armed;
  int receive_ready;
  int receive_error;
  int send_pending;

  uint8_t recv_buf[LDAP_RECV_BUFFER_SIZE];
  size_t recv_buf_used;

  ldap_message_t *pending_response;
  int pending_result;
  int response_received;
  int expected_message_id;

  ldap_search_cb search_callback;
  void *search_user_data;
  ldap_result_data_t *search_result;
};

typedef struct {
  const char *dn;
  const char *password;
  ldap_result_data_t *result;
} ldap_bind_args_t;

typedef struct {
  const ldap_search_params_t *params;
  ldap_search_cb callback;
  void *user_data;
  ldap_result_data_t *result;
} ldap_search_args_t;

typedef struct {
  const char *dn;
  const ldap_attribute_t *attrs;
  size_t attr_count;
  ldap_result_data_t *result;
} ldap_add_args_t;

typedef struct {
  const char *dn;
  ldap_result_data_t *result;
} ldap_delete_args_t;

typedef struct {
  const char *dn;
  const ldap_modification_t *mods;
  size_t mod_count;
  ldap_result_data_t *result;
} ldap_modify_args_t;

typedef struct {
  const char *dn;
  const char *new_rdn;
  const char *new_parent;
  int delete_old_rdn;
  ldap_result_data_t *result;
} ldap_rename_args_t;

typedef struct {
  const char *dn;
  const char *attr;
  const char *value;
  size_t value_len;
  ldap_result_data_t *result;
} ldap_compare_args_t;

static native_io_backend_kind ldap_client_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

static char *ldap_client_duplicate(const char *value, size_t length) {
  char *copy = (char *)malloc(length + 1u);
  if (copy == NULL) return NULL;
  memcpy(copy, value, length);
  copy[length] = '\0';
  return copy;
}

static int ldap_client_parse_port(const char *first, const char *last, uint16_t *port) {
  unsigned int value = 0u;
  const char *cursor;
  if (first == last) return -1;
  for (cursor = first; cursor != last; ++cursor) {
    unsigned int digit;
    if (*cursor < '0' || *cursor > '9') return -1;
    digit = (unsigned int)(*cursor - '0');
    if (value > (65535u - digit) / 10u) return -1;
    value = value * 10u + digit;
  }
  if (value == 0u) return -1;
  *port = (uint16_t)value;
  return 0;
}

static int parse_url(const char *url, char **host, uint16_t *port, int *use_tls) {
  const char *cursor;
  const char *authority_end;
  const char *host_first;
  const char *host_last;
  const char *port_first = NULL;

  if (url == NULL || host == NULL || port == NULL || use_tls == NULL) return -1;
  if (strlen(url) >= LDAP_CNET_URI_CAPACITY) return -1;
  *host = NULL;
  *use_tls = 0;
  *port = LDAP_DEFAULT_PORT;
  cursor = url;

  if (strncmp(cursor, "ldaps://", 8u) == 0) {
    *use_tls = 1;
    *port = LDAP_DEFAULT_TLS_PORT;
    cursor += 8;
  } else if (strncmp(cursor, "ldap://", 7u) == 0) {
    cursor += 7;
  } else if (strstr(cursor, "://") != NULL) {
    return -1;
  }

  authority_end = strchr(cursor, '/');
  if (authority_end == NULL) authority_end = cursor + strlen(cursor);
  if (cursor == authority_end) return -1;

  if (*cursor == '[') {
    const char *closing = memchr(cursor + 1, ']', (size_t)(authority_end - cursor - 1));
    if (closing == NULL || closing == cursor + 1) return -1;
    host_first = cursor + 1;
    host_last = closing;
    if (closing + 1 != authority_end) {
      if (closing[1] != ':') return -1;
      port_first = closing + 2;
    }
  } else {
    const char *colon = memchr(cursor, ':', (size_t)(authority_end - cursor));
    host_first = cursor;
    host_last = colon != NULL ? colon : authority_end;
    if (colon != NULL) port_first = colon + 1;
  }

  if (host_first == host_last) return -1;
  if (port_first != NULL && ldap_client_parse_port(port_first, authority_end, port) != 0) return -1;
  *host = ldap_client_duplicate(host_first, (size_t)(host_last - host_first));
  return *host != NULL ? 0 : -1;
}

static void ldap_client_set_transport_error(ldap_client_t *client, const char *operation,
                                            int status) {
  client->transport_status = status;
  (void)snprintf(client->error_msg, sizeof(client->error_msg), "%s failed with CNet status %d",
                 operation, status);
}

static void ldap_client_on_state(void *user, cnet_connection connection,
                                 cnet_connection_state state, const cnet_error *error) {
  ldap_client_t *client = (ldap_client_t *)user;
  if (connection.slot != client->connection.slot ||
      connection.generation != client->connection.generation) return;
  if (state == CNET_CONNECTION_CONNECTED) {
    client->connected = 1;
  } else if (state == CNET_CONNECTION_CLOSED || state == CNET_CONNECTION_FAILED) {
    client->connected = 0;
    client->terminal = 1;
    client->terminal_failed = state == CNET_CONNECTION_FAILED;
    if (client->transport_status == SALTS_OK) {
      client->transport_status = error != NULL ? error->status : SALTS_EIO;
    }
  }
}

static int ldap_client_append_recv(ldap_client_t *client, const void *data, size_t len) {
  if (!client || !data || len == 0) return 0;
  if (len > LDAP_RECV_BUFFER_SIZE - client->recv_buf_used) return LDAP_CLIENT_ERROR_PROTOCOL;
  memcpy(client->recv_buf + client->recv_buf_used, data, len);
  client->recv_buf_used += len;
  return LDAP_CLIENT_OK;
}

static void ldap_client_on_receive(void *user, cnet_connection connection,
                                   const cnet_receive_view *view) {
  ldap_client_t *client = (ldap_client_t *)user;
  int status;
  if (connection.slot != client->connection.slot ||
      connection.generation != client->connection.generation) return;
  client->receive_armed = 0;
  if (view == NULL || view->kind != CNET_MESSAGE_BYTES ||
      (view->size != 0u && view->data == NULL)) {
    client->receive_error = LDAP_CLIENT_ERROR_PROTOCOL;
    ldap_client_set_transport_error(client, "receive", SALTS_EPROTO);
    return;
  }
  status = ldap_client_append_recv(client, view->data, view->size);
  if (status != LDAP_CLIENT_OK) {
    client->receive_error = status;
    ldap_client_set_transport_error(client, "receive buffer", SALTS_EMSGSIZE);
    return;
  }
  client->receive_ready = 1;
}

static void ldap_client_on_send(void *user, cnet_connection connection, size_t size) {
  ldap_client_t *client = (ldap_client_t *)user;
  if (connection.slot != client->connection.slot ||
      connection.generation != client->connection.generation) return;
  (void)size;
  client->send_pending = 0;
}

static void ldap_client_recycle(void *user) {
  ldap_client_t *client = (ldap_client_t *)user;
  client->managed = (cnet_managed_connection){0};
}

/* The attachment borrows client through terminal and post-callback recycle.
 * Keep protocol result storage independent; it may outlive the TCP stream. */
static int ldap_client_collect(ldap_client_t *client) {
  cnet_manager_snapshot snapshot;
  size_t work = 0u;
  int status;
  if (!client->manager.impl) return SALTS_OK;
  status = cnet_manager_advance(&client->manager, 1u, &work);
  if (status != SALTS_OK) return status;
  status = cnet_manager_get_snapshot(&client->manager, &snapshot);
  if (status != SALTS_OK) return status;
  return snapshot.drained ? cnet_manager_destroy(&client->manager) : SALTS_OK;
}

static int ldap_client_progress(ldap_client_t *client, uint32_t wait_ms) {
  size_t events = 0u;
  int status = cnet_client_poll(&client->net, wait_ms, &events);
  int cleanup = ldap_client_collect(client);
  return status != SALTS_OK ? status : cleanup;
}

static void ldap_client_consume_recv(ldap_client_t *client, size_t consumed) {
  if (!client || consumed == 0 || consumed > client->recv_buf_used) return;

  if (consumed < client->recv_buf_used) {
    memmove(client->recv_buf, client->recv_buf + consumed, client->recv_buf_used - consumed);
  }
  client->recv_buf_used -= consumed;
}

static void ldap_client_finish_with_result(ldap_client_t *client, int result_code) {
  if (!client) return;
  client->pending_result = result_code;
  client->response_received = 1;
}

static int ldap_client_process_message(ldap_client_t *client, ldap_message_t *msg) {
  if (!client || !msg) return LDAP_CLIENT_ERROR_PROTOCOL;

  if (msg->message_id != client->expected_message_id) {
    ldap_message_free(msg);
    return LDAP_CLIENT_OK;
  }

  switch (msg->protocol_op) {
  case LDAP_RES_SEARCH_ENTRY:
    if (client->search_callback) {
      client->search_callback(client, &msg->payload.search_entry, client->search_user_data);
    }
    ldap_message_free(msg);
    return LDAP_CLIENT_OK;

  case LDAP_RES_SEARCH_DONE:
    client->last_result_code = msg->payload.search_done.result_code;
    if (client->search_result) {
      *client->search_result = msg->payload.search_done;
      memset(&msg->payload.search_done, 0, sizeof(ldap_result_data_t));
    }
    ldap_message_free(msg);
    ldap_client_finish_with_result(client, LDAP_CLIENT_OK);
    return LDAP_CLIENT_OK;

  default:
    client->pending_response = msg;
    switch (msg->protocol_op) {
    case LDAP_RES_BIND:
      client->last_result_code = msg->payload.bind_response.result_code;
      break;
    case LDAP_RES_MODIFY:
    case LDAP_RES_ADD:
    case LDAP_RES_DELETE:
    case LDAP_RES_MODDN:
    case LDAP_RES_COMPARE:
    case LDAP_RES_SEARCH_REF:
      client->last_result_code = msg->payload.generic_result.result_code;
      break;
    default:
      client->last_result_code = 0;
      break;
    }
    ldap_client_finish_with_result(client, LDAP_CLIENT_OK);
    return LDAP_CLIENT_OK;
  }
}

static int ldap_client_drain_recv_buffer(ldap_client_t *client) {
  while (client && client->recv_buf_used > 0) {
    ldap_parse_result_t parse_result;
    int complete = ldap_message_complete(client->recv_buf, client->recv_buf_used);
    if (complete < 0) return LDAP_CLIENT_ERROR_PROTOCOL;
    if (complete == 0) return LDAP_CLIENT_OK;

    memset(&parse_result, 0, sizeof(parse_result));
    int rc = ldap_parse_message(client->recv_buf, client->recv_buf_used, &parse_result);
    if (rc != LDAP_PARSE_OK || !parse_result.message) return LDAP_CLIENT_ERROR_PROTOCOL;

    rc = ldap_client_process_message(client, parse_result.message);
    ldap_client_consume_recv(client, parse_result.bytes_consumed);
    if (rc != LDAP_CLIENT_OK) return rc;
    if (client->response_received) return LDAP_CLIENT_OK;
  }

  return LDAP_CLIENT_OK;
}

static uint32_t ldap_client_remaining_ms(uint64_t deadline) {
  const uint64_t now = cmeta_monotonic_ms();
  const uint64_t remaining = deadline > now ? deadline - now : 0u;
  return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

static int ldap_client_recv_until_response(ldap_client_t *client, uint64_t deadline) {
  while (client != NULL && !client->response_received) {
    int status;
    uint32_t wait_ms;

    if (client->receive_error != LDAP_CLIENT_OK) return client->receive_error;
    if (client->receive_ready) {
      client->receive_ready = 0;
      status = ldap_client_drain_recv_buffer(client);
      if (status != LDAP_CLIENT_OK) {
        (void)snprintf(client->error_msg, sizeof(client->error_msg), "LDAP parse/protocol failure");
        return status;
      }
      if (client->response_received) break;
    }
    if (client->terminal) {
      ldap_client_set_transport_error(client, "receive", client->transport_status);
      return LDAP_CLIENT_ERROR_NETWORK;
    }
    if (!client->receive_armed) {
      status = cnet_receive(&client->net, client->connection, 1u);
      if (status != SALTS_OK) {
        ldap_client_set_transport_error(client, "receive admission", status);
        return LDAP_CLIENT_ERROR_NETWORK;
      }
      client->receive_armed = 1;
    }

    wait_ms = ldap_client_remaining_ms(deadline);
    if (wait_ms == 0u) {
      (void)snprintf(client->error_msg, sizeof(client->error_msg), "Operation timeout");
      return LDAP_CLIENT_ERROR_TIMEOUT;
    }
    status = ldap_client_progress(client, wait_ms);
    if (status != SALTS_OK) {
      ldap_client_set_transport_error(client, "request poll", status);
      return status == SALTS_ETIMEDOUT ? LDAP_CLIENT_ERROR_TIMEOUT : LDAP_CLIENT_ERROR_NETWORK;
    }
  }

  return client != NULL ? client->pending_result : LDAP_CLIENT_ERROR_INVALID;
}

static int ldap_client_send_and_wait(ldap_client_t *client, const uint8_t *data, size_t len,
                                     int message_id) {
  uint64_t deadline;
  int status;
  if (!client || !client->connected) {
    if (client) {
      (void)snprintf(client->error_msg, sizeof(client->error_msg), "Not connected");
    }
    return LDAP_CLIENT_ERROR_NETWORK;
  }

  if (client->pending_response) {
    ldap_message_free(client->pending_response);
    client->pending_response = NULL;
  }

  client->pending_result = LDAP_CLIENT_ERROR_TIMEOUT;
  client->response_received = 0;
  client->expected_message_id = message_id;
  client->receive_error = LDAP_CLIENT_OK;
  client->receive_ready = 0;

  if (!client->receive_armed) {
    status = cnet_receive(&client->net, client->connection, 1u);
    if (status != SALTS_OK) {
      ldap_client_set_transport_error(client, "receive admission", status);
      return LDAP_CLIENT_ERROR_NETWORK;
    }
    client->receive_armed = 1;
  }

  client->send_pending = 1;
  status = ldap_cnet_send_bytes(
      &client->net, client->connection, data, len, 0);
  if (status != SALTS_OK) {
    client->send_pending = 0;
    ldap_client_set_transport_error(client, "send admission", status);
    return LDAP_CLIENT_ERROR_NETWORK;
  }
  deadline = cmeta_monotonic_ms() + client->timeout_ms;
  return ldap_client_recv_until_response(client, deadline);
}

static int ldap_client_connect_impl(ldap_client_t *client) {
  cnet_connect_options options;
  const char *format;
  char uri[LDAP_CNET_URI_CAPACITY];
  uint64_t deadline;
  int uri_size;
  int status;

  if (client == NULL || !client->net_initialized) return LDAP_CLIENT_ERROR_INVALID;
  if (client->connected) return LDAP_CLIENT_OK;

  /* A timed-out connect may still owe a real terminal. Finish that episode
   * before resetting callback storage or attempting another connection. */
  deadline = cmeta_monotonic_ms() + client->timeout_ms;
  status = ldap_client_collect(client);
  if (status == SALTS_OK && client->manager.impl) {
    status = cnet_close(&client->net, client->connection);
    if (status == SALTS_EALREADY || status == SALTS_ENOENT) status = SALTS_OK;
    while (status == SALTS_OK && client->manager.impl) {
      uint32_t wait_ms = ldap_client_remaining_ms(deadline);
      if (wait_ms == 0u) { status = SALTS_ETIMEDOUT; break; }
      status = ldap_client_progress(client, wait_ms);
    }
  }
  if (status != SALTS_OK) {
    ldap_client_set_transport_error(client, "previous connect drain", status);
    return status == SALTS_ETIMEDOUT ? LDAP_CLIENT_ERROR_TIMEOUT : LDAP_CLIENT_ERROR_NETWORK;
  }

  format = strchr(client->host, ':') != NULL ? (client->use_tls ? "tls://[%s]:%u" : "tcp://[%s]:%u")
                                             : (client->use_tls ? "tls://%s:%u" : "tcp://%s:%u");
  uri_size = snprintf(uri, sizeof(uri), format, client->host, (unsigned int)client->port);
  if (uri_size < 0 || (size_t)uri_size >= sizeof(uri)) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "LDAP URL is too long");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  client->connected = 0;
  client->terminal = 0;
  client->terminal_failed = 0;
  client->transport_status = SALTS_OK;
  client->receive_armed = 0;
  client->receive_ready = 0;
  client->receive_error = LDAP_CLIENT_OK;
  client->send_pending = 0;
  client->recv_buf_used = 0u;
  client->connection = (cnet_connection){0};
  options = (cnet_connect_options){.uri = uri,
                                   .observer = {.on_state = ldap_client_on_state,
                                                .on_receive = ldap_client_on_receive,
                                                .user = client,
                                                .on_send = ldap_client_on_send}};
  {
    cnet_manager_config config = {sizeof(config), CNET_MANAGER_VERSION, &client->net, 1u, 1u};
    cnet_manager_attachment attachment = {.observer = options.observer,
                                           .on_recycle = ldap_client_recycle};
    status = cnet_manager_init(&client->manager, &config);
    if (status == SALTS_OK)
      status = cnet_manager_reserve(&client->manager, &attachment, &client->managed);
    if (status == SALTS_OK)
      status = cnet_manager_connect(&client->manager, client->managed, &options, &client->connection);
  }
  if (status != SALTS_OK) {
    int cleanup = ldap_client_collect(client);
    if (cleanup != SALTS_OK) status = cleanup;
    ldap_client_set_transport_error(client, "connect admission", status);
    return client->use_tls ? LDAP_CLIENT_ERROR_TLS : LDAP_CLIENT_ERROR_NETWORK;
  }

  while (!client->connected && !client->terminal) {
    const uint32_t wait_ms = ldap_client_remaining_ms(deadline);
    if (wait_ms == 0u) {
      (void)cnet_close(&client->net, client->connection);
      (void)snprintf(client->error_msg, sizeof(client->error_msg), "Operation timeout");
      return LDAP_CLIENT_ERROR_TIMEOUT;
    }
    status = ldap_client_progress(client, wait_ms);
    if (status != SALTS_OK) {
      ldap_client_set_transport_error(client, "connect poll", status);
      return status == SALTS_ETIMEDOUT
                 ? LDAP_CLIENT_ERROR_TIMEOUT
                 : (client->use_tls ? LDAP_CLIENT_ERROR_TLS : LDAP_CLIENT_ERROR_NETWORK);
    }
  }
  if (client->terminal) {
    ldap_client_set_transport_error(client, "connect", client->transport_status);
    return client->transport_status == SALTS_ETIMEDOUT
               ? LDAP_CLIENT_ERROR_TIMEOUT
               : (client->use_tls ? LDAP_CLIENT_ERROR_TLS : LDAP_CLIENT_ERROR_NETWORK);
  }
  return LDAP_CLIENT_OK;
}

static void ldap_client_transfer_pending(ldap_client_t *client, ldap_result_data_t *result,
                                         int is_bind) {
  if (!client || !client->pending_response) return;

  ldap_message_t *msg = client->pending_response;
  client->pending_response = NULL;

  if (result) {
    if (is_bind) {
      *result = msg->payload.bind_response;
      memset(&msg->payload.bind_response, 0, sizeof(ldap_result_data_t));
    } else {
      *result = msg->payload.generic_result;
      memset(&msg->payload.generic_result, 0, sizeof(ldap_result_data_t));
    }
  }

  ldap_message_free(msg);
}

static int ldap_client_simple_bind_impl(ldap_client_t *client, void *opaque) {
  ldap_bind_args_t *args = (ldap_bind_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_bind_request(message_id, 3, args->dn, args->password, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build BindRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  rc = ldap_client_send_and_wait(client, buf, len, message_id);
  if (rc != LDAP_CLIENT_OK) return rc;

  if (client->pending_response) {
    client->last_result_code = client->pending_response->payload.bind_response.result_code;
    ldap_client_transfer_pending(client, args->result, 1);
  }

  return (client->last_result_code == LDAP_SUCCESS) ? LDAP_CLIENT_OK : LDAP_CLIENT_ERROR_AUTH;
}

static int ldap_client_search_impl(ldap_client_t *client, void *opaque) {
  ldap_search_args_t *args = (ldap_search_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->params) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_search_request(message_id, args->params->base_dn, args->params->scope, 0,
                                 args->params->size_limit, args->params->time_limit,
                                 args->params->types_only ? 1 : 0, args->params->filter,
                                 args->params->attrs, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build SearchRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  client->search_callback = args->callback;
  client->search_user_data = args->user_data;
  client->search_result = args->result;

  rc = ldap_client_send_and_wait(client, buf, len, message_id);

  client->search_callback = NULL;
  client->search_user_data = NULL;
  client->search_result = NULL;
  return rc;
}

static int ldap_client_add_impl(ldap_client_t *client, void *opaque) {
  ldap_add_args_t *args = (ldap_add_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_add_request(message_id, args->dn, args->attrs, args->attr_count, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build AddRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  rc = ldap_client_send_and_wait(client, buf, len, message_id);
  if (rc != LDAP_CLIENT_OK) return rc;

  if (client->pending_response) {
    client->last_result_code = client->pending_response->payload.generic_result.result_code;
    ldap_client_transfer_pending(client, args->result, 0);
  }

  return LDAP_CLIENT_OK;
}

static int ldap_client_delete_impl(ldap_client_t *client, void *opaque) {
  ldap_delete_args_t *args = (ldap_delete_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_delete_request(message_id, args->dn, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build DeleteRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  rc = ldap_client_send_and_wait(client, buf, len, message_id);
  if (rc != LDAP_CLIENT_OK) return rc;

  if (client->pending_response) {
    client->last_result_code = client->pending_response->payload.generic_result.result_code;
    ldap_client_transfer_pending(client, args->result, 0);
  }

  return LDAP_CLIENT_OK;
}

static int ldap_client_modify_impl(ldap_client_t *client, void *opaque) {
  ldap_modify_args_t *args = (ldap_modify_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_modify_request(message_id, args->dn, args->mods, args->mod_count, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build ModifyRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  rc = ldap_client_send_and_wait(client, buf, len, message_id);
  if (rc != LDAP_CLIENT_OK) return rc;

  if (client->pending_response) {
    client->last_result_code = client->pending_response->payload.generic_result.result_code;
    ldap_client_transfer_pending(client, args->result, 0);
  }

  return LDAP_CLIENT_OK;
}

static int ldap_client_rename_impl(ldap_client_t *client, void *opaque) {
  ldap_rename_args_t *args = (ldap_rename_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn || !args->new_rdn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_modifydn_request(message_id, args->dn, args->new_rdn, args->delete_old_rdn,
                                   args->new_parent, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build ModifyDNRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  rc = ldap_client_send_and_wait(client, buf, len, message_id);
  if (rc != LDAP_CLIENT_OK) return rc;

  if (client->pending_response) {
    client->last_result_code = client->pending_response->payload.generic_result.result_code;
    ldap_client_transfer_pending(client, args->result, 0);
  }

  return LDAP_CLIENT_OK;
}

static int ldap_client_compare_impl(ldap_client_t *client, void *opaque) {
  ldap_compare_args_t *args = (ldap_compare_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn || !args->attr || !args->value)
    return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_impl(client);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_compare_request(message_id, args->dn, args->attr, (const uint8_t *)args->value,
                                  args->value_len, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    (void)snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build CompareRequest");
    return LDAP_CLIENT_ERROR_INVALID;
  }

  rc = ldap_client_send_and_wait(client, buf, len, message_id);
  if (rc != LDAP_CLIENT_OK) return rc;

  if (client->pending_response) {
    client->last_result_code = client->pending_response->payload.generic_result.result_code;
    ldap_client_transfer_pending(client, args->result, 0);
  }

  return LDAP_CLIENT_OK;
}

static int ldap_client_unbind_impl(ldap_client_t *client) {
  uint8_t buf[64];
  size_t len = sizeof(buf);
  uint64_t deadline;
  int message_id;
  int status;

  if (!client || !client->connected) return LDAP_CLIENT_ERROR_INVALID;

  message_id = client->next_message_id++;
  if (ldap_build_unbind_request(message_id, buf, &len) != LDAP_BUILD_OK) {
    return LDAP_CLIENT_ERROR_INVALID;
  }

  client->terminal = 0;
  client->terminal_failed = 0;
  client->transport_status = SALTS_OK;
  client->send_pending = 1;
  status = ldap_cnet_send_bytes(
      &client->net, client->connection, buf, len, 1);
  if (status != SALTS_OK) {
    client->send_pending = 0;
    ldap_client_set_transport_error(client, "unbind admission", status);
    return LDAP_CLIENT_ERROR_NETWORK;
  }

  deadline = cmeta_monotonic_ms() + client->timeout_ms;
  while (!client->terminal) {
    const uint32_t wait_ms = ldap_client_remaining_ms(deadline);
    if (wait_ms == 0u) {
      (void)snprintf(client->error_msg, sizeof(client->error_msg), "Operation timeout");
      return LDAP_CLIENT_ERROR_TIMEOUT;
    }
    status = ldap_client_progress(client, wait_ms);
    if (status != SALTS_OK) {
      ldap_client_set_transport_error(client, "unbind poll", status);
      return status == SALTS_ETIMEDOUT ? LDAP_CLIENT_ERROR_TIMEOUT : LDAP_CLIENT_ERROR_NETWORK;
    }
  }

  if (client->terminal_failed) {
    ldap_client_set_transport_error(client, "unbind", client->transport_status);
    return LDAP_CLIENT_ERROR_NETWORK;
  }
  client->connected = 0;
  client->receive_armed = 0;
  client->recv_buf_used = 0u;
  return LDAP_CLIENT_OK;
}

ldap_client_t *ldap_client_create(const ldap_client_config_t *config) {
  ldap_client_t *client;
  cnet_client_config net_config;
  int status;

  client = (ldap_client_t *)calloc(1, sizeof(*client));
  if (!client) return NULL;

  if (config && config->url) {
    if (parse_url(config->url, &client->host, &client->port, &client->use_tls) != 0) {
      free(client);
      return NULL;
    }
  } else {
    client->host = ldap_client_duplicate("localhost", sizeof("localhost") - 1u);
    client->port = LDAP_DEFAULT_PORT;
    if (!client->host) {
      free(client);
      return NULL;
    }
  }

  client->timeout_ms =
      (config && config->timeout_ms > 0) ? (uint32_t)config->timeout_ms : LDAP_DEFAULT_TIMEOUT_MS;
  client->next_message_id = 1;
  net_config = (cnet_client_config){.backend = ldap_client_backend(),
                                    .connection_capacity = 1u,
                                    .command_capacity = LDAP_CNET_QUEUE_CAPACITY,
                                    .request_capacity = LDAP_CNET_QUEUE_CAPACITY,
                                    .completion_batch_capacity = LDAP_CNET_QUEUE_CAPACITY,
                                    .event_capacity = LDAP_CNET_QUEUE_CAPACITY,
                                    .max_send_bytes = LDAP_SEND_BUFFER_SIZE,
                                    .receive_buffer_bytes = LDAP_RECV_BUFFER_SIZE,
                                    .connect_timeout_ms = client->timeout_ms,
                                    .read_timeout_ms = client->timeout_ms,
                                    .write_timeout_ms = client->timeout_ms,
                                    .tls_io_buffer_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES,
                                    .tls_handshake_timeout_ms = client->timeout_ms,
                                    .command_buffer_bytes = LDAP_SEND_BUFFER_SIZE,
                                    .event_buffer_bytes = LDAP_RECV_BUFFER_SIZE};
  status = cnet_client_init(&client->net, &net_config);
  if (status != SALTS_OK) {
    free(client->host);
    free(client);
    return NULL;
  }
  client->net_initialized = 1;
  return client;
}

void ldap_client_destroy(ldap_client_t *client) {
  if (!client) return;

  if (client->net_initialized) {
    if (client->manager.impl && cnet_manager_request_close(&client->manager) != SALTS_OK) return;
    if (cnet_client_stop(&client->net, LDAP_CNET_STOP_TIMEOUT_MS) != SALTS_OK) return;
    if (ldap_client_collect(client) != SALTS_OK || client->manager.impl) return;
    if (cnet_client_destroy(&client->net) != SALTS_OK) return;
  }
  if (client->pending_response) {
    ldap_message_free(client->pending_response);
  }

  free(client->host);
  free(client);
}

int ldap_client_connect(ldap_client_t *client) {
  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  return ldap_client_connect_impl(client);
}

int ldap_client_simple_bind(ldap_client_t *client, const char *dn, const char *password,
                            ldap_result_data_t *result) {
  ldap_bind_args_t args;
  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  args.dn = dn ? dn : "";
  args.password = password ? password : "";
  args.result = result;
  return ldap_client_simple_bind_impl(client, &args);
}

int ldap_client_unbind(ldap_client_t *client) {
  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  return ldap_client_unbind_impl(client);
}

int ldap_client_search(ldap_client_t *client, const ldap_search_params_t *params,
                       ldap_search_cb callback, void *user_data, ldap_result_data_t *result) {
  ldap_search_args_t args;
  args.params = params;
  args.callback = callback;
  args.user_data = user_data;
  args.result = result;
  return ldap_client_search_impl(client, &args);
}

int ldap_client_add(ldap_client_t *client, const char *dn, const ldap_attribute_t *attrs,
                    size_t attr_count, ldap_result_data_t *result) {
  ldap_add_args_t args;
  args.dn = dn;
  args.attrs = attrs;
  args.attr_count = attr_count;
  args.result = result;
  return ldap_client_add_impl(client, &args);
}

int ldap_client_delete(ldap_client_t *client, const char *dn, ldap_result_data_t *result) {
  ldap_delete_args_t args;
  args.dn = dn;
  args.result = result;
  return ldap_client_delete_impl(client, &args);
}

int ldap_client_modify(ldap_client_t *client, const char *dn, const ldap_modification_t *mods,
                       size_t mod_count, ldap_result_data_t *result) {
  ldap_modify_args_t args;
  args.dn = dn;
  args.mods = mods;
  args.mod_count = mod_count;
  args.result = result;
  return ldap_client_modify_impl(client, &args);
}

int ldap_client_rename(ldap_client_t *client, const char *dn, const char *new_rdn,
                       const char *new_parent, int delete_old_rdn, ldap_result_data_t *result) {
  ldap_rename_args_t args;
  args.dn = dn;
  args.new_rdn = new_rdn;
  args.new_parent = new_parent;
  args.delete_old_rdn = delete_old_rdn;
  args.result = result;
  return ldap_client_rename_impl(client, &args);
}

int ldap_client_compare(ldap_client_t *client, const char *dn, const char *attr, const char *value,
                        size_t value_len, ldap_result_data_t *result) {
  ldap_compare_args_t args;
  args.dn = dn;
  args.attr = attr;
  args.value = value;
  args.value_len = value_len;
  args.result = result;
  return ldap_client_compare_impl(client, &args);
}

const char *ldap_err2string(int err) {
  switch (err) {
  case LDAP_CLIENT_OK:
    return "Success";
  case LDAP_CLIENT_ERROR_MEMORY:
    return "Memory allocation failed";
  case LDAP_CLIENT_ERROR_INVALID:
    return "Invalid parameter";
  case LDAP_CLIENT_ERROR_NETWORK:
    return "Network error";
  case LDAP_CLIENT_ERROR_TIMEOUT:
    return "Operation timeout";
  case LDAP_CLIENT_ERROR_PROTOCOL:
    return "Protocol error";
  case LDAP_CLIENT_ERROR_AUTH:
    return "Authentication failed";
  case LDAP_CLIENT_ERROR_TLS:
    return "TLS error";
  default:
    return "Unknown error";
  }
}
