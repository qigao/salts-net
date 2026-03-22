/**
 * @file ldap_client.c
 * @brief LDAP Client implementation on top of CoroNet coroutine sockets.
 */

#include "ldap_client.h"
#include "ldap_builder.h"
#include "ldap_parser.h"
#include <CoroNet/turbo_coro_context.h>
#include <CoroNet/turbo_coro_socket.h>
#include <stb_sprintf.h>
#include <stdlib.h>
#include <string.h>

/* Client error codes (match ldap_client.h) */
#define LDAP_CLIENT_OK              0
#define LDAP_CLIENT_ERROR_MEMORY   -1
#define LDAP_CLIENT_ERROR_INVALID  -2
#define LDAP_CLIENT_ERROR_NETWORK  -3
#define LDAP_CLIENT_ERROR_TIMEOUT  -4
#define LDAP_CLIENT_ERROR_PROTOCOL -5
#define LDAP_CLIENT_ERROR_AUTH     -6
#define LDAP_CLIENT_ERROR_TLS      -7

#define LDAP_DEFAULT_PORT 389
#define LDAP_DEFAULT_TIMEOUT_MS 30000
#define LDAP_RECV_BUFFER_SIZE 65536
#define LDAP_SEND_BUFFER_SIZE 4096

typedef int (*ldap_coro_op_fn)(ldap_client_t *client, void *arg);

typedef struct {
  ldap_client_t *client;
  ldap_coro_op_fn fn;
  void *arg;
  int result;
} ldap_sync_op_t;

struct ldap_client_s {
  char *host;
  uint16_t port;
  uint32_t timeout_ms;

  int32_t next_message_id;
  char error_msg[256];
  int last_result_code;

  coro_context_t *ctx;
  coro_socket_t *socket;
  int connected;

  uint8_t *recv_buf;
  size_t recv_buf_size;
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

static int parse_url(const char *url, char **host, uint16_t *port, int *use_tls) {
  const char *cursor = url;
  const char *colon;
  const char *slash;
  size_t host_len;

  *use_tls = 0;
  *port = LDAP_DEFAULT_PORT;

  if (!url || !host || !port || !use_tls) return -1;

  if (strncmp(cursor, "ldaps://", 8) == 0) {
    *use_tls = 1;
    *port = 636;
    cursor += 8;
  } else if (strncmp(cursor, "ldap://", 7) == 0) {
    cursor += 7;
  }

  colon = strchr(cursor, ':');
  slash = strchr(cursor, '/');

  if (colon && (!slash || colon < slash)) {
    host_len = (size_t)(colon - cursor);
    *port = (uint16_t)atoi(colon + 1);
  } else if (slash) {
    host_len = (size_t)(slash - cursor);
  } else {
    host_len = strlen(cursor);
  }

  *host = (char *)malloc(host_len + 1);
  if (!*host) return -1;

  memcpy(*host, cursor, host_len);
  (*host)[host_len] = '\0';
  return 0;
}

static int ldap_client_append_recv(ldap_client_t *client, const char *data, size_t len) {
  if (!client || !data || len == 0) return 0;

  if (client->recv_buf_used + len > client->recv_buf_size) {
    size_t new_size = client->recv_buf_size ? client->recv_buf_size : LDAP_RECV_BUFFER_SIZE;
    while (new_size < client->recv_buf_used + len) {
      new_size *= 2;
    }

    uint8_t *new_buf = (uint8_t *)realloc(client->recv_buf, new_size);
    if (!new_buf) return LDAP_CLIENT_ERROR_MEMORY;
    client->recv_buf = new_buf;
    client->recv_buf_size = new_size;
  }

  memcpy(client->recv_buf + client->recv_buf_used, data, len);
  client->recv_buf_used += len;
  return LDAP_CLIENT_OK;
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
    if (complete <= 0) return LDAP_CLIENT_OK;

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

static int ldap_client_recv_until_response(ldap_client_t *client) {
  while (client && !client->response_received) {
    char *chunk = NULL;
    size_t chunk_len = 0;
    int rc = coro_socket_recv(client->socket, &chunk, &chunk_len);

    if (rc == TURBO_ETIMEDOUT) {
      if (chunk) coro_socket_free_recv(chunk);
      stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Operation timeout");
      return LDAP_CLIENT_ERROR_TIMEOUT;
    }
    if (rc != 0) {
      if (chunk) coro_socket_free_recv(chunk);
      client->connected = 0;
      stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Receive failed (%d)", rc);
      return LDAP_CLIENT_ERROR_NETWORK;
    }

    if (chunk && chunk_len > 0) {
      rc = ldap_client_append_recv(client, chunk, chunk_len);
      coro_socket_free_recv(chunk);
      if (rc != LDAP_CLIENT_OK) {
        stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Receive buffer allocation failed");
        return rc;
      }
      rc = ldap_client_drain_recv_buffer(client);
      if (rc != LDAP_CLIENT_OK) {
        stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "LDAP parse/protocol failure");
        return rc;
      }
    } else if (chunk) {
      coro_socket_free_recv(chunk);
    }
  }

  return client ? client->pending_result : LDAP_CLIENT_ERROR_INVALID;
}

static int ldap_client_send_and_wait(ldap_client_t *client, const uint8_t *data, size_t len,
                                     int message_id) {
  if (!client || !client->connected || !client->socket) {
    stbsp_snprintf(client ? client->error_msg : NULL, client ? sizeof(client->error_msg) : 0,
                   "Not connected");
    return LDAP_CLIENT_ERROR_NETWORK;
  }

  if (client->pending_response) {
    ldap_message_free(client->pending_response);
    client->pending_response = NULL;
  }

  client->pending_result = LDAP_CLIENT_ERROR_TIMEOUT;
  client->response_received = 0;
  client->expected_message_id = message_id;

  int rc = coro_socket_send(client->socket, (const char *)data, len);
  if (rc == TURBO_ETIMEDOUT) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Operation timeout");
    return LDAP_CLIENT_ERROR_TIMEOUT;
  }
  if (rc != 0) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Send failed (%d)", rc);
    return LDAP_CLIENT_ERROR_NETWORK;
  }

  return ldap_client_recv_until_response(client);
}

static void ldap_sync_entry(coro_t *co, void *arg) {
  (void)co;
  ldap_sync_op_t *op = (ldap_sync_op_t *)arg;
  op->result = op->fn(op->client, op->arg);
  coro_context_stop(op->client->ctx);
}

static int ldap_client_run_sync(ldap_client_t *client, ldap_coro_op_fn fn, void *arg) {
  ldap_sync_op_t op;
  int rc;

  if (!client || !client->ctx || !fn) return LDAP_CLIENT_ERROR_INVALID;

  op.client = client;
  op.fn = fn;
  op.arg = arg;
  op.result = LDAP_CLIENT_ERROR_INVALID;

  rc = coro_context_spawn(client->ctx, ldap_sync_entry, &op);
  if (rc != 0) return LDAP_CLIENT_ERROR_MEMORY;

  coro_context_run(client->ctx, TURBO_RUN_DEFAULT);
  return op.result;
}

static int ldap_client_connect_coro(ldap_client_t *client, void *arg) {
  (void)arg;

  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  if (client->connected && client->socket) return LDAP_CLIENT_OK;

  if (client->socket) {
    coro_socket_destroy(client->socket);
    client->socket = NULL;
  }

  client->socket = coro_socket_create(client->ctx, CORO_SOCKET_TCP_V4);
  if (!client->socket) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to create socket");
    return LDAP_CLIENT_ERROR_MEMORY;
  }

  coro_socket_set_timeout(client->socket, client->timeout_ms);

  int rc = coro_socket_connect(client->socket, client->host, client->port);
  if (rc == TURBO_ETIMEDOUT) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Operation timeout");
    coro_socket_destroy(client->socket);
    client->socket = NULL;
    return LDAP_CLIENT_ERROR_TIMEOUT;
  }
  if (rc != 0) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Connect failed (%d)", rc);
    coro_socket_destroy(client->socket);
    client->socket = NULL;
    return LDAP_CLIENT_ERROR_NETWORK;
  }

  client->connected = 1;
  client->recv_buf_used = 0;
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

static int ldap_client_simple_bind_coro(ldap_client_t *client, void *opaque) {
  ldap_bind_args_t *args = (ldap_bind_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_bind_request(message_id, 3, args->dn, args->password, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build BindRequest");
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

static int ldap_client_search_coro(ldap_client_t *client, void *opaque) {
  ldap_search_args_t *args = (ldap_search_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->params) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_search_request(message_id, args->params->base_dn, args->params->scope, 0,
                                 args->params->size_limit, args->params->time_limit,
                                 args->params->types_only ? 1 : 0, args->params->filter,
                                 args->params->attrs, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build SearchRequest");
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

static int ldap_client_add_coro(ldap_client_t *client, void *opaque) {
  ldap_add_args_t *args = (ldap_add_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_add_request(message_id, args->dn, args->attrs, args->attr_count, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build AddRequest");
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

static int ldap_client_delete_coro(ldap_client_t *client, void *opaque) {
  ldap_delete_args_t *args = (ldap_delete_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_delete_request(message_id, args->dn, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build DeleteRequest");
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

static int ldap_client_modify_coro(ldap_client_t *client, void *opaque) {
  ldap_modify_args_t *args = (ldap_modify_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_modify_request(message_id, args->dn, args->mods, args->mod_count, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build ModifyRequest");
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

static int ldap_client_rename_coro(ldap_client_t *client, void *opaque) {
  ldap_rename_args_t *args = (ldap_rename_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn || !args->new_rdn) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_modifydn_request(message_id, args->dn, args->new_rdn, args->delete_old_rdn,
                                   args->new_parent, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build ModifyDNRequest");
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

static int ldap_client_compare_coro(ldap_client_t *client, void *opaque) {
  ldap_compare_args_t *args = (ldap_compare_args_t *)opaque;
  uint8_t buf[LDAP_SEND_BUFFER_SIZE];
  size_t len = sizeof(buf);
  int message_id;
  int rc;

  if (!client || !args || !args->dn || !args->attr || !args->value) return LDAP_CLIENT_ERROR_INVALID;

  rc = ldap_client_connect_coro(client, NULL);
  if (rc != LDAP_CLIENT_OK) return rc;

  message_id = client->next_message_id++;
  rc = ldap_build_compare_request(message_id, args->dn, args->attr,
                                  (const uint8_t *)args->value, args->value_len, buf, &len);
  if (rc != LDAP_BUILD_OK) {
    stbsp_snprintf(client->error_msg, sizeof(client->error_msg), "Failed to build CompareRequest");
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

static int ldap_client_unbind_coro(ldap_client_t *client, void *arg) {
  uint8_t buf[64];
  size_t len = sizeof(buf);
  int message_id;

  (void)arg;

  if (!client || !client->connected || !client->socket) return LDAP_CLIENT_ERROR_INVALID;

  message_id = client->next_message_id++;
  if (ldap_build_unbind_request(message_id, buf, &len) != LDAP_BUILD_OK) {
    return LDAP_CLIENT_ERROR_INVALID;
  }

  if (coro_socket_send(client->socket, (const char *)buf, len) != 0) {
    return LDAP_CLIENT_ERROR_NETWORK;
  }

  coro_socket_destroy(client->socket);
  client->socket = NULL;
  client->connected = 0;
  client->recv_buf_used = 0;
  return LDAP_CLIENT_OK;
}

ldap_client_t *ldap_client_create(const ldap_client_config_t *config) {
  ldap_client_t *client;
  int use_tls = 0;

  client = (ldap_client_t *)calloc(1, sizeof(*client));
  if (!client) return NULL;

  if (config && config->url) {
    if (parse_url(config->url, &client->host, &client->port, &use_tls) != 0) {
      free(client);
      return NULL;
    }
  } else {
    client->host = strdup("localhost");
    client->port = LDAP_DEFAULT_PORT;
    if (!client->host) {
      free(client);
      return NULL;
    }
  }

  client->timeout_ms = (config && config->timeout_ms > 0) ? (uint32_t)config->timeout_ms
                                                          : LDAP_DEFAULT_TIMEOUT_MS;
  client->next_message_id = 1;
  client->recv_buf_size = LDAP_RECV_BUFFER_SIZE;
  client->recv_buf = (uint8_t *)malloc(client->recv_buf_size);
  if (!client->recv_buf) {
    free(client->host);
    free(client);
    return NULL;
  }

  client->ctx = coro_context_create(NULL);
  if (!client->ctx) {
    free(client->recv_buf);
    free(client->host);
    free(client);
    return NULL;
  }

  (void)use_tls;
  return client;
}

void ldap_client_destroy(ldap_client_t *client) {
  if (!client) return;

  if (client->pending_response) {
    ldap_message_free(client->pending_response);
  }
  if (client->socket) {
    coro_socket_destroy(client->socket);
  }
  if (client->ctx) {
    coro_context_destroy(client->ctx);
  }

  free(client->recv_buf);
  free(client->host);
  free(client);
}

int ldap_client_connect(ldap_client_t *client) {
  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  return ldap_client_run_sync(client, ldap_client_connect_coro, NULL);
}

int ldap_client_simple_bind(ldap_client_t *client, const char *dn, const char *password,
                            ldap_result_data_t *result) {
  ldap_bind_args_t args;
  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  args.dn = dn ? dn : "";
  args.password = password ? password : "";
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_simple_bind_coro, &args);
}

int ldap_client_unbind(ldap_client_t *client) {
  if (!client) return LDAP_CLIENT_ERROR_INVALID;
  return ldap_client_run_sync(client, ldap_client_unbind_coro, NULL);
}

int ldap_client_search(ldap_client_t *client, const ldap_search_params_t *params,
                       ldap_search_cb callback, void *user_data, ldap_result_data_t *result) {
  ldap_search_args_t args;
  args.params = params;
  args.callback = callback;
  args.user_data = user_data;
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_search_coro, &args);
}

int ldap_client_add(ldap_client_t *client, const char *dn, const ldap_attribute_t *attrs,
                    size_t attr_count, ldap_result_data_t *result) {
  ldap_add_args_t args;
  args.dn = dn;
  args.attrs = attrs;
  args.attr_count = attr_count;
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_add_coro, &args);
}

int ldap_client_delete(ldap_client_t *client, const char *dn, ldap_result_data_t *result) {
  ldap_delete_args_t args;
  args.dn = dn;
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_delete_coro, &args);
}

int ldap_client_modify(ldap_client_t *client, const char *dn, const ldap_modification_t *mods,
                       size_t mod_count, ldap_result_data_t *result) {
  ldap_modify_args_t args;
  args.dn = dn;
  args.mods = mods;
  args.mod_count = mod_count;
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_modify_coro, &args);
}

int ldap_client_rename(ldap_client_t *client, const char *dn, const char *new_rdn,
                       const char *new_parent, int delete_old_rdn,
                       ldap_result_data_t *result) {
  ldap_rename_args_t args;
  args.dn = dn;
  args.new_rdn = new_rdn;
  args.new_parent = new_parent;
  args.delete_old_rdn = delete_old_rdn;
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_rename_coro, &args);
}

int ldap_client_compare(ldap_client_t *client, const char *dn, const char *attr,
                        const char *value, size_t value_len, ldap_result_data_t *result) {
  ldap_compare_args_t args;
  args.dn = dn;
  args.attr = attr;
  args.value = value;
  args.value_len = value_len;
  args.result = result;
  return ldap_client_run_sync(client, ldap_client_compare_coro, &args);
}

const char *ldap_err2string(int err) {
  switch (err) {
    case LDAP_CLIENT_OK:             return "Success";
    case LDAP_CLIENT_ERROR_MEMORY:   return "Memory allocation failed";
    case LDAP_CLIENT_ERROR_INVALID:  return "Invalid parameter";
    case LDAP_CLIENT_ERROR_NETWORK:  return "Network error";
    case LDAP_CLIENT_ERROR_TIMEOUT:  return "Operation timeout";
    case LDAP_CLIENT_ERROR_PROTOCOL: return "Protocol error";
    case LDAP_CLIENT_ERROR_AUTH:     return "Authentication failed";
    case LDAP_CLIENT_ERROR_TLS:      return "TLS error";
    default:                         return "Unknown error";
  }
}
