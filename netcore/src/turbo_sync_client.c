#include "stb_sprintf.h"
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include <uv.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include "arena_buffer.h"
#include "client_common.h"
#include "config.h"
#include "tlog.h"
#include "turbo_dns.h"
#include "turbo_kcp.h"
#include "turbo_pipe.h"
#include "turbo_sync_client.h"
#include "turbo_tcp.h"
#include "turbo_tls.h"
#include "turbo_url.h"
#include "turbo_websocket_client.h"


#define SYNC_CLIENT_ERROR_MESSAGE_MAX 128
#define UNUSED(x) (void)(x)

#ifndef CONTAINER_OF
  #define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

typedef enum {
  COMMAND_CONNECT,
  COMMAND_SEND,
  COMMAND_SENDV,
  COMMAND_RECEIVE,
  COMMAND_STOP
} sync_client_command_type_t;

typedef struct sync_client_command_s {
  sync_client_command_type_t type;
  union {
    struct {
      char *host;
      int port;
      sync_client_transport_t transport;
    } connect;
    struct {
      char *data;
      size_t len;
    } send;
    struct {
      sync_client_iovec_t *iov;
      size_t iovcnt;
    } sendv;
  } payload;
  struct sync_client_command_s *next;
} sync_client_command_t;

typedef struct {
  turbo_tcp_client_t *client;
  int connected;
} sync_client_tcp_state_t;

typedef struct {
  uv_udp_t handle;
  uv_udp_send_t send_req;
  struct sockaddr_storage remote_addr;
  int remote_addr_len;
  int connected;
  int recv_active;
} sync_client_udp_state_t;

typedef struct {
  turbo_kcp_client_t client;
  int initialized;
} sync_client_kcp_state_t;

typedef struct {
  turbo_tls_context_t context;
  turbo_tls_client_t *client;
  int context_initialized;
  int client_created;
  int handshake_pending;
  int connected;
} sync_client_tls_state_t;

typedef struct {
  turbo_pipe_client_t *client;
} sync_client_pipe_state_t;

typedef struct {
  turbo_websocket_client_t *client;
  sync_client_ws_config_t config;
  int connected;
  /* Owned copies to avoid dangling stack pointers from caller config */
  char *path_owned;
  char *origin_owned;
  char **subprotocols_owned;
  int subprotocols_owned_count;

  /* TLS context for wss:// */
  turbo_tls_context_t tls_context;
  int tls_context_initialized;
} sync_client_ws_state_t;

/* ========================================================================
 * Transport operations vtable - eliminates all protocol switch statements
 *
 * Each protocol (TCP/UDP/KCP/TLS/PIPE) implements this interface.
 * Main code calls client->ops->xxx() without knowing which protocol.
 *
 * This is "good taste" - no special cases, just polymorphism.
 * ======================================================================== */

typedef struct sync_client_s sync_client_t;

typedef struct sync_client_transport_ops_s {
  /* Initialize transport-specific state */
  int (*setup)(sync_client_t *client);

  /* Connect to remote host:port */
  int (*connect)(sync_client_t *client, const char *host, int port);

  /* Send single buffer */
  int (*send)(sync_client_t *client, const char *data, size_t len);

  /* Send scatter-gather IOV array */
  int (*sendv)(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);

  /* Cleanup and close connection */
  void (*close)(sync_client_t *client);

  /* Optional protocol-specific methods (NULL if not supported) */
  int (*receive)(sync_client_t *client); /* TCP-specific: check/wait for received data */

  /* Protocol name for debugging */
  const char *name;
} sync_client_transport_ops_t;

/* Forward declarations for ops functions we'll implement below */
static int tcp_setup_impl(sync_client_t *client);
static int tcp_connect_impl(sync_client_t *client, const char *host, int port);
static int tcp_send_impl(sync_client_t *client, const char *data, size_t len);
static int tcp_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void tcp_close_impl(sync_client_t *client);
static int tcp_receive_impl(sync_client_t *client);

static int udp_setup_impl(sync_client_t *client);
static int udp_connect_impl(sync_client_t *client, const char *host, int port);
static int udp_send_impl(sync_client_t *client, const char *data, size_t len);
static int udp_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void udp_close_impl(sync_client_t *client);

static int kcp_setup_impl(sync_client_t *client);
static int kcp_connect_impl(sync_client_t *client, const char *host, int port);
static int kcp_send_impl(sync_client_t *client, const char *data, size_t len);
static int kcp_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void kcp_close_impl(sync_client_t *client);

static int tls_setup_impl(sync_client_t *client);
static int tls_connect_impl(sync_client_t *client, const char *host, int port);
static int tls_send_impl(sync_client_t *client, const char *data, size_t len);
static int tls_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void tls_close_impl(sync_client_t *client);

static int pipe_setup_impl(sync_client_t *client);
static int pipe_connect_impl(sync_client_t *client, const char *host, int port);
static int pipe_send_impl(sync_client_t *client, const char *data, size_t len);
static int pipe_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void pipe_close_impl(sync_client_t *client);

static int ws_setup_impl(sync_client_t *client);
static int ws_connect_impl(sync_client_t *client, const char *host, int port);
static int ws_send_impl(sync_client_t *client, const char *data, size_t len);
static int ws_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void ws_close_impl(sync_client_t *client);

/* TCP operations */
static const sync_client_transport_ops_t tcp_ops = {.setup = tcp_setup_impl,
                                                    .connect = tcp_connect_impl,
                                                    .send = tcp_send_impl,
                                                    .sendv = tcp_sendv_impl,
                                                    .close = tcp_close_impl,
                                                    .receive = tcp_receive_impl,
                                                    .name = "TCP"};

/* UDP operations */
static const sync_client_transport_ops_t udp_ops = {.setup = udp_setup_impl,
                                                    .connect = udp_connect_impl,
                                                    .send = udp_send_impl,
                                                    .sendv = udp_sendv_impl,
                                                    .close = udp_close_impl,
                                                    .receive = NULL,
                                                    .name = "UDP"};

/* KCP operations */
static const sync_client_transport_ops_t kcp_ops = {.setup = kcp_setup_impl,
                                                    .connect = kcp_connect_impl,
                                                    .send = kcp_send_impl,
                                                    .sendv = kcp_sendv_impl,
                                                    .close = kcp_close_impl,
                                                    .receive = NULL,
                                                    .name = "KCP"};

/* TLS operations */
static const sync_client_transport_ops_t tls_ops = {.setup = tls_setup_impl,
                                                    .connect = tls_connect_impl,
                                                    .send = tls_send_impl,
                                                    .sendv = tls_sendv_impl,
                                                    .close = tls_close_impl,
                                                    .receive = NULL,
                                                    .name = "TLS"};

/* PIPE operations */
static const sync_client_transport_ops_t pipe_ops = {.setup = pipe_setup_impl,
                                                     .connect = pipe_connect_impl,
                                                     .send = pipe_send_impl,
                                                     .sendv = pipe_sendv_impl,
                                                     .close = pipe_close_impl,
                                                     .receive = NULL,
                                                     .name = "PIPE"};

/* WebSocket operations */
static const sync_client_transport_ops_t websocket_ops = {.setup = ws_setup_impl,
                                                          .connect = ws_connect_impl,
                                                          .send = ws_send_impl,
                                                          .sendv = ws_sendv_impl,
                                                          .close = ws_close_impl,
                                                          .receive = NULL,
                                                          .name = "WebSocket"};

struct sync_client_s {
  uv_loop_t loop;
  uv_thread_t loop_thread;
  uv_async_t command_async;

  uv_mutex_t mutex;
  uv_cond_t cond;

  sync_client_command_t *command_head;
  sync_client_command_t *command_tail;

  sync_client_transport_t transport;
  const sync_client_transport_ops_t *ops; /* Function pointer table - no more switch! */

  /* Phase 3: Union optimization - save ~140 bytes per client (1.4MB for 10k connections)
   * Only one protocol is active at a time, so we use a union instead of 5 separate fields */
  union {
    sync_client_tcp_state_t tcp;
    sync_client_udp_state_t udp;
    sync_client_kcp_state_t kcp;
    sync_client_tls_state_t tls;
    sync_client_pipe_state_t pipe;
    sync_client_ws_state_t ws;
  } proto;

  sync_client_status_t result_code;
  int uv_status;
  char error_message[SYNC_CLIENT_ERROR_MESSAGE_MAX];
  char *response;
  size_t response_len;
  int done;
  int awaiting_receive;
  char *pending_send_data;
  size_t pending_send_len;

  int loop_ready;
  int loop_running;
  int shutting_down;
  int init_error;
  int config_acquired;

  /* New fields for enhancements */
  sync_client_state_t state;
  int connect_timeout_ms;
  int operation_timeout_ms;

  /* Statistics */
  sync_client_stats_t stats;

  struct sync_client_s *global_next;
};

static void result_reset(sync_client_t *client);
static void result_set_error(sync_client_t *client, sync_client_status_t status,
                             const char *message);
static void result_set_uv_error(sync_client_t *client, int uv_status);
static void result_set_transport_error(sync_client_t *client, const char *message);

static sync_client_command_t *command_create(sync_client_command_type_t type);
static sync_client_command_t *command_create_connect(sync_client_transport_t transport,
                                                     const char *host, int port);
static sync_client_command_t *command_create_send(const char *data, size_t len);
static sync_client_command_t *command_create_receive(void);
static sync_client_command_t *command_create_stop(void);
static void free_command(sync_client_command_t *cmd);

static sync_client_status_t submit_command(sync_client_t *client, sync_client_command_t *cmd,
                                           int reset_response, int mark_shutdown);

static void command_async_cb(uv_async_t *handle);
static void loop_thread_main(void *arg);

static void client_list_register(sync_client_t *client);
static void client_list_unregister(sync_client_t *client);
static sync_client_t *client_from_pipe(turbo_pipe_client_t *pipe_client);

static void transport_close(sync_client_t *client);
static sync_client_status_t transport_setup(sync_client_t *client);

static void handle_connect_command(sync_client_t *client, sync_client_command_t *cmd);
static void handle_send_command(sync_client_t *client, sync_client_command_t *cmd);
static void handle_sendv_command(sync_client_t *client, sync_client_command_t *cmd);
static void handle_receive_command(sync_client_t *client);
static void handle_stop_command(sync_client_t *client);

/* Helper functions for Phase 1 refactoring */
static void cleanup_iov_buffers(sync_client_iovec_t *iov, size_t iovcnt);
static void signal_done_with_status(sync_client_t *client, int rc, sync_client_status_t error_code,
                                    const char *error_msg);
static void update_send_stats_locked(sync_client_t *client, size_t bytes_sent, size_t iovcnt);
static void update_error_stats_locked(sync_client_t *client);
static size_t calculate_iov_total_bytes(const sync_client_iovec_t *iov, size_t iovcnt);

static void tcp_client_connect_cb(turbo_tcp_client_t *tcp_client, int status, void *peer);
static int tcp_client_recv_cb(turbo_tcp_client_t *tcp_client, const turbo_arena_slice_t *data,
                              void *peer);
static void tcp_client_close_cb(void *handle);

static void udp_send_cb(uv_udp_send_t *req, int status);
static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void udp_read_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags);

static void kcp_connect_cb(turbo_kcp_client_t *kcp_client, int status, void *peer);
static int kcp_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);

static void tls_connect_cb(turbo_tls_client_t *tls_client, int status, void *peer);
static void tls_close_cb(void *handle);
static void tls_handshake_cb(turbo_tls_client_t *tls_client, int status);
static int tls_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);

static int pipe_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);
static void pipe_connect_cb(turbo_pipe_client_t *pipe_client, int status, void *peer);
static void pipe_close_cb(void *handle);
CLIENT_COMMON_DEFINE_PIPE_CLIENT_LIST(sync, sync_client_t);
#define g_client_list_once g_sync_client_list_once
#define g_client_list_lock g_sync_client_list_lock
#define g_client_list_initialized g_sync_client_list_initialized
#define g_client_list_head g_sync_client_list_head

const char *sync_client_status_to_string(sync_client_status_t status) {
  switch (status) {
  case SYNC_CLIENT_STATUS_OK:
    return "ok";
  case SYNC_CLIENT_STATUS_INVALID_PARAM:
    return "invalid parameter";
  case SYNC_CLIENT_STATUS_ALLOC_FAILED:
    return "allocation failure";
  case SYNC_CLIENT_STATUS_NOT_READY:
    return "client not ready";
  case SYNC_CLIENT_STATUS_SHUTTING_DOWN:
    return "client shutting down";
  case SYNC_CLIENT_STATUS_IO_ERROR:
    return "I/O error";
  case SYNC_CLIENT_STATUS_TRANSPORT_ERROR:
    return "transport error";
  case SYNC_CLIENT_STATUS_INTERNAL_ERROR:
    return "internal error";
  default:
    return "unknown error";
  }
}

const char *sync_client_transport_to_string(sync_client_transport_t transport) {
  switch (transport) {
  case SYNC_CLIENT_TRANSPORT_TCP:
    return "tcp";
  case SYNC_CLIENT_TRANSPORT_UDP:
    return "udp";
  case SYNC_CLIENT_TRANSPORT_KCP:
    return "kcp";
  case SYNC_CLIENT_TRANSPORT_TLS:
    return "tls";
  case SYNC_CLIENT_TRANSPORT_PIPE:
    return "pipe";
  case SYNC_CLIENT_TRANSPORT_WEBSOCKET:
    return "websocket";
  default:
    return "unknown";
  }
}

sync_client_status_t sync_client_last_status(sync_client_t *client) {
  if (!client)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;
  return client->result_code;
}

int sync_client_last_uv_error(sync_client_t *client) {
  if (!client)
    return 0;
  return client->uv_status;
}

const char *sync_client_last_message(sync_client_t *client) {
  if (!client)
    return "client not available";
  if (client->error_message[0] != '\0')
    return client->error_message;
  return sync_client_status_to_string(client->result_code);
}

const char *sync_client_get_transport_scheme(const sync_client_t *client) {
  if (!client || !client->ops) {
    return "unknown";
  }
  return sync_client_transport_to_string(client->transport);
}

static void result_reset(sync_client_t *client) {
  client->result_code = SYNC_CLIENT_STATUS_OK;
  client->uv_status = 0;
  client->error_message[0] = '\0';
}
static void result_set_error(sync_client_t *client, sync_client_status_t status,
                             const char *message) {
  client->result_code = status;
  client->uv_status = 0;
  if (message && message[0] != '\0') {
    strncpy(client->error_message, message, sizeof(client->error_message) - 1);
    client->error_message[sizeof(client->error_message) - 1] = '\0';
  } else {
    client->error_message[0] = '\0';
  }
}

static void result_set_uv_error(sync_client_t *client, int uv_status) {
  client->result_code = SYNC_CLIENT_STATUS_IO_ERROR;
  client->uv_status = uv_status;
  const char *msg = uv_strerror(uv_status);
  if (msg) {
    /* Copy to padded buffer to avoid ASan false positives from stb_sprintf 4-byte reads */
    char msg_padded[128] = {0};
    strncpy(msg_padded, msg, sizeof(msg_padded) - 1);
    static const char FMT_NET_ERR_MSG[32] = "network error: %s (%d)";
    stbsp_snprintf(client->error_message, (int)sizeof(client->error_message), FMT_NET_ERR_MSG,
                   msg_padded, uv_status);
  } else {
    static const char FMT_NET_ERR_CODE[32] = "network error (code %d)";
    stbsp_snprintf(client->error_message, (int)sizeof(client->error_message), FMT_NET_ERR_CODE,
                   uv_status);
  }
}

static void result_set_transport_error(sync_client_t *client, const char *message) {
  client->result_code = SYNC_CLIENT_STATUS_TRANSPORT_ERROR;
  client->uv_status = 0;
  if (message && message[0] != '\0') {
    strncpy(client->error_message, message, sizeof(client->error_message) - 1);
    client->error_message[sizeof(client->error_message) - 1] = '\0';
  } else {
    client->error_message[0] = '\0';
  }
}

static void client_list_init_once(void) {
  if (uv_mutex_init(&g_client_list_lock) == 0)
    g_client_list_initialized = 1;
}

static void client_list_register(sync_client_t *client) {
  uv_once(&g_client_list_once, client_list_init_once);
  if (!g_client_list_initialized)
    return;
  uv_mutex_lock(&g_client_list_lock);
  client->global_next = g_client_list_head;
  g_client_list_head = client;
  uv_mutex_unlock(&g_client_list_lock);
}

static void client_list_unregister(sync_client_t *client) {
  if (!g_client_list_initialized)
    return;
  uv_mutex_lock(&g_client_list_lock);
  sync_client_t **node = &g_client_list_head;
  while (*node) {
    if (*node == client) {
      *node = client->global_next;
      break;
    }
    node = &(*node)->global_next;
  }
  client->global_next = NULL;
  uv_mutex_unlock(&g_client_list_lock);
}

static sync_client_t *client_from_pipe(turbo_pipe_client_t *pipe_client) {
  if (!g_client_list_initialized)
    return NULL;
  uv_mutex_lock(&g_client_list_lock);
  sync_client_t *cursor = g_client_list_head;
  while (cursor) {
    if (cursor->transport == SYNC_CLIENT_TRANSPORT_PIPE &&
        cursor->proto.pipe.client == pipe_client) {
      uv_mutex_unlock(&g_client_list_lock);
      return cursor;
    }
    cursor = cursor->global_next;
  }
  uv_mutex_unlock(&g_client_list_lock);
  return NULL;
}

static sync_client_command_t *command_create(sync_client_command_type_t type) {
  sync_client_command_t *cmd = (sync_client_command_t *)calloc(1, sizeof(*cmd));
  if (!cmd)
    return NULL;
  cmd->type = type;
  return cmd;
}

static sync_client_command_t *command_create_connect(sync_client_transport_t transport,
                                                     const char *host, int port) {
  if (!host)
    return NULL;
  sync_client_command_t *cmd = command_create(COMMAND_CONNECT);
  if (!cmd)
    return NULL;
  cmd->payload.connect.transport = transport;
  cmd->payload.connect.host = client_common_strdup(host);
  if (!cmd->payload.connect.host) {
    free(cmd);
    return NULL;
  }
  cmd->payload.connect.port = port;
  return cmd;
}

static sync_client_command_t *command_create_send(const char *data, size_t len) {
  sync_client_command_t *cmd = command_create(COMMAND_SEND);
  if (!cmd)
    return NULL;
  if (len > 0) {
    cmd->payload.send.data = (char *)malloc(len);
    if (!cmd->payload.send.data) {
      free(cmd);
      return NULL;
    }
    memcpy(cmd->payload.send.data, data, len);
    cmd->payload.send.len = len;
  }
  return cmd;
}

static sync_client_command_t *command_create_sendv(const sync_client_iovec_t *iov, size_t iovcnt) {
  if (!iov || iovcnt == 0)
    return NULL;

  sync_client_command_t *cmd = command_create(COMMAND_SENDV);
  if (!cmd)
    return NULL;

  /* Allocate iovec array */
  cmd->payload.sendv.iov = (sync_client_iovec_t *)malloc(iovcnt * sizeof(sync_client_iovec_t));
  if (!cmd->payload.sendv.iov) {
    free(cmd);
    return NULL;
  }

  cmd->payload.sendv.iovcnt = iovcnt;

  /* Copy each buffer */
  for (size_t i = 0; i < iovcnt; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      cmd->payload.sendv.iov[i].data = (char *)malloc(iov[i].len);
      if (!cmd->payload.sendv.iov[i].data) {
        /* Cleanup on failure */
        for (size_t j = 0; j < i; j++) {
          free((void *)cmd->payload.sendv.iov[j].data);
        }
        free(cmd->payload.sendv.iov);
        free(cmd);
        return NULL;
      }
      memcpy((void *)cmd->payload.sendv.iov[i].data, iov[i].data, iov[i].len);
      cmd->payload.sendv.iov[i].len = iov[i].len;
    } else {
      cmd->payload.sendv.iov[i].data = NULL;
      cmd->payload.sendv.iov[i].len = 0;
    }
  }

  return cmd;
}

static sync_client_command_t *command_create_receive(void) {
  return command_create(COMMAND_RECEIVE);
}

static sync_client_command_t *command_create_stop(void) { return command_create(COMMAND_STOP); }

static void free_command(sync_client_command_t *cmd) {
  if (!cmd)
    return;
  if (cmd->type == COMMAND_CONNECT) {
    free(cmd->payload.connect.host);
  } else if (cmd->type == COMMAND_SEND && cmd->payload.send.data) {
    free(cmd->payload.send.data);
  } else if (cmd->type == COMMAND_SENDV && cmd->payload.sendv.iov) {
    for (size_t i = 0; i < cmd->payload.sendv.iovcnt; i++) {
      free((void *)cmd->payload.sendv.iov[i].data);
    }
    free(cmd->payload.sendv.iov);
  }
  free(cmd);
}

static sync_client_status_t submit_command(sync_client_t *client, sync_client_command_t *cmd,
                                           int reset_response, int mark_shutdown) {
  sync_client_status_t rc = SYNC_CLIENT_STATUS_OK;

  uv_mutex_lock(&client->mutex);
  if (!client->loop_running) {
    rc = SYNC_CLIENT_STATUS_NOT_READY;
    result_set_error(client, rc, NULL);
  } else if (client->shutting_down && !mark_shutdown) {
    rc = SYNC_CLIENT_STATUS_SHUTTING_DOWN;
    result_set_error(client, rc, NULL);
  } else {
    if (mark_shutdown)
      client->shutting_down = 1;
    client->done = 0;
    result_reset(client);
    if (reset_response && client->response) {
      free(client->response);
      client->response = NULL;
      client->response_len = 0;
    }
    cmd->next = NULL;
    if (client->command_tail)
      client->command_tail->next = cmd;
    else
      client->command_head = cmd;
    client->command_tail = cmd;
  }
  uv_mutex_unlock(&client->mutex);

  if (rc == SYNC_CLIENT_STATUS_OK)
    uv_async_send(&client->command_async);

  return rc;
}

static void transport_close(sync_client_t *client) {
  /* If ops is not set, transport was never initialized (no connect called) */
  if (!client->ops) {
    return;
  }

  /* Dispatch via vtable - NO SWITCH! */
  if (client->ops->close)
    client->ops->close(client);
}

static sync_client_status_t transport_setup(sync_client_t *client) {
  /* Dispatch via vtable - NO SWITCH! */
  return client->ops->setup(client);
}
static void handle_connect_command(sync_client_t *client, sync_client_command_t *cmd) {
  sync_client_transport_t transport = cmd->payload.connect.transport;
  const char *host = cmd->payload.connect.host;
  int port = cmd->payload.connect.port;

  /* Initialize transport-specific ops if not already done */
  if (!client->ops) {
    client->transport = transport;
    switch (transport) {
    case SYNC_CLIENT_TRANSPORT_TCP:
      client->ops = &tcp_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_UDP:
      client->ops = &udp_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_KCP:
      client->ops = &kcp_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_TLS:
      client->ops = &tls_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_PIPE:
      client->ops = &pipe_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_WEBSOCKET:
      client->ops = &websocket_ops;
      break;
    default:
      result_set_error(client, SYNC_CLIENT_STATUS_TRANSPORT_ERROR, "unsupported transport");
      client->done = 1;
      uv_cond_signal(&client->cond);
      return;
    }

    int setup_rc = client->ops->setup(client);
    if (setup_rc != SYNC_CLIENT_STATUS_OK) {
      client->ops = NULL;
      result_set_error(client, setup_rc, "transport setup failed");
      client->done = 1;
      uv_cond_signal(&client->cond);
      return;
    }
  } else if (client->transport != transport) {
    result_set_error(client, SYNC_CLIENT_STATUS_TRANSPORT_ERROR,
                     "client already initialized with different transport");
    client->done = 1;
    uv_cond_signal(&client->cond);
    return;
  }

  /* Dispatch via vtable - NO 210-LINE SWITCH! This is "good taste". */
  client->ops->connect(client, host, port);
}
static void handle_send_command(sync_client_t *client, sync_client_command_t *cmd) {
  char *data = cmd->payload.send.data;
  size_t len = cmd->payload.send.len;
  TLOG_DEBUG("handle_send_command: transport={} data={} len={}", (int)client->transport,
             (void *)data, len);

  if (len == 0 || !data) {
    signal_done_with_status(client, 0, SYNC_CLIENT_STATUS_OK, NULL);
    return;
  }

  /* Transfer ownership */
  cmd->payload.send.data = NULL;

  /* Dispatch via vtable - NO SWITCH STATEMENT! */
  int rc;
  if (client->ops && client->ops->send) {
    rc = client->ops->send(client, data, len);
  } else {
    rc = -1; /* Not ready */
  }
  TLOG_DEBUG("handle_send_command: ops->send returned rc={}", rc);

  /* UDP handles cleanup in callback */
  if (client->transport != SYNC_CLIENT_TRANSPORT_UDP) {
    free(data);
  }

  /* Update stats and signal completion */
  uv_mutex_lock(&client->mutex);
  if (rc == 0) {
    result_reset(client);
    client->stats.bytes_sent += len;
    client->stats.messages_sent++;
  } else {
    /* rc == -1 means "not connected/not ready" from transport layer
     * Other negative values are libuv error codes */
    if (rc == -1) {
      result_set_error(client, SYNC_CLIENT_STATUS_NOT_READY, "transport not ready");
    } else if (rc < 0) {
      result_set_uv_error(client, rc);
    }
    client->stats.send_errors++;
  }
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

/* ========================================================================
 * Helper functions to eliminate code duplication in send/sendv commands
 * ======================================================================== */

/**
 * Cleanup IOV buffers - frees both data and array.
 * Eliminates the repeated 6-line cleanup pattern.
 */
static void cleanup_iov_buffers(sync_client_iovec_t *iov, size_t iovcnt) {
  TLOG_DEBUG("cleanup_iov_buffers: iov={} iovcnt={}", (void *)iov, iovcnt);
  if (!iov) {
    return;
  }
  for (size_t i = 0; i < iovcnt; i++) {
    TLOG_DEBUG("cleanup_iov_buffers: freeing iov[{}].data={}", i, (void *)iov[i].data);
    free((void *)iov[i].data);
  }
  free(iov);
}

/**
 * Signal command completion with optional error.
 * Eliminates the repeated mutex/signal/unlock pattern.
 */
static void signal_done_with_status(sync_client_t *client, int rc, sync_client_status_t error_code,
                                    const char *error_msg) {
  uv_mutex_lock(&client->mutex);
  if (rc == 0) {
    result_reset(client);
  } else if (rc == -1) {
    /* Custom error code */
    result_set_error(client, error_code, error_msg);
  } else if (rc < 0 && rc > -4096) {
    /* libuv error code */
    result_set_uv_error(client, rc);
  } else {
    result_set_error(client, error_code, error_msg);
  }
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

/**
 * Update stats after successful send.
 * Eliminates repeated 4-line stats update pattern.
 */
static void update_send_stats_locked(sync_client_t *client, size_t bytes_sent, size_t iovcnt) {
  client->stats.bytes_sent += bytes_sent;
  client->stats.messages_sent++;
  client->stats.scatter_gather_sends++;
  client->stats.total_iov_buffers_sent += iovcnt;
}

/**
 * Update error stats.
 */
static void update_error_stats_locked(sync_client_t *client) { client->stats.send_errors++; }

/**
 * Calculate total bytes in IOV array.
 */
static size_t calculate_iov_total_bytes(const sync_client_iovec_t *iov, size_t iovcnt) {
  size_t total = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    total += iov[i].len;
  }
  return total;
}

/* ========================================================================
 * Protocol-specific sendv implementations
 * ======================================================================== */

/**
 * TCP scatter-gather send implementation.
 * Uses zero-copy arena with queue+flush pattern.
 */
static int tcp_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.tcp.client || !client->proto.tcp.connected) {
    return -1; /* Caller handles error */
  }

  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_tcp_get_send_buffer(client->proto.tcp.client, iov[i].len);
      if (!buffer) {
        return UV_ENOMEM;
      }
      memcpy(buffer->data, iov[i].data, iov[i].len);
      turbo_arena_buffer_set_used(buffer, iov[i].len);
      rc = turbo_tcp_queue_buffer(client->proto.tcp.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }

  if (rc == 0) {
    rc = turbo_tcp_flush(client->proto.tcp.client);
  }
  return rc;
}

/**
 * UDP scatter-gather send implementation.
 * Uses native uv_udp_send with IOV array.
 */
static int udp_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.udp.connected && client->proto.udp.remote_addr_len == 0) {
    return -1;
  }

  uv_buf_t *udp_bufs = (uv_buf_t *)malloc(iovcnt * sizeof(uv_buf_t));
  if (!udp_bufs) {
    return UV_ENOMEM;
  }

  for (size_t i = 0; i < iovcnt; i++) {
    udp_bufs[i] = uv_buf_init((char *)iov[i].data, (unsigned int)iov[i].len);
  }

  client->proto.udp.send_req.data = client;
  int rc = uv_udp_send(
      &client->proto.udp.send_req, &client->proto.udp.handle, udp_bufs, (unsigned int)iovcnt,
      client->proto.udp.connected ? NULL : (const struct sockaddr *)&client->proto.udp.remote_addr,
      udp_send_cb);

  free(udp_bufs);

  if (rc == 0) {
    /* Store iov for cleanup in callback */
    client->pending_send_data = (char *)iov;
    client->pending_send_len = iovcnt;

    /* Update stats now for UDP (callback will only cleanup) */
    size_t total_bytes = calculate_iov_total_bytes(iov, iovcnt);
    uv_mutex_lock(&client->mutex);
    update_send_stats_locked(client, total_bytes, iovcnt);
    uv_mutex_unlock(&client->mutex);
  }
  return rc;
}

/**
 * KCP scatter-gather send implementation.
 * Uses zero-copy wrapper.
 */
static int kcp_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.kcp.initialized) {
    return -1;
  }

  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_arena_wrap_external((void *)iov[i].data, iov[i].len, NULL, NULL);
      if (!buffer) {
        return UV_ENOMEM;
      }
      rc = turbo_kcp_client_send_buffer(&client->proto.kcp.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

/**
 * TLS scatter-gather send implementation.
 * Uses arena allocator with auto-flush.
 */
static int tls_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.tls.client || !client->proto.tls.client_created ||
      !client->proto.tls.connected) {
    return -1;
  }

  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_tls_get_send_buffer(client->proto.tls.client, iov[i].len);
      if (!buffer) {
        return UV_ENOMEM;
      }
      memcpy(buffer->data, iov[i].data, iov[i].len);
      turbo_arena_buffer_set_used(buffer, iov[i].len);
      rc = turbo_tls_send_buffer(client->proto.tls.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

/**
 * PIPE scatter-gather send implementation.
 * Uses arena allocator with manual flush.
 */
static int pipe_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.pipe.client) {
    return -1;
  }

  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_pipe_get_send_buffer(client->proto.pipe.client, iov[i].len);
      if (!buffer) {
        return UV_ENOMEM;
      }
      memcpy(buffer->data, iov[i].data, iov[i].len);
      turbo_arena_buffer_set_used(buffer, iov[i].len);
      rc = turbo_pipe_send_buffer(client->proto.pipe.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }

  if (rc == 0) {
    rc = turbo_pipe_flush(client->proto.pipe.client);
  }
  return rc;
}

/* ========================================================================
 * Transport operations tables (vtable) - Phase 2a: send/sendv only
 *
 * NOTE: Only send/sendv are implemented in Phase 2a.
 * setup/connect/close still use switch statements for now.
 * This demonstrates the vtable approach with minimal risk.
 * ======================================================================== */

/* Forward declarations for ops functions we'll implement below */
static int tcp_setup_impl(sync_client_t *client);
static int tcp_connect_impl(sync_client_t *client, const char *host, int port);
static int tcp_send_impl(sync_client_t *client, const char *data, size_t len);
static void tcp_close_impl(sync_client_t *client);
static int tcp_receive_impl(sync_client_t *client); /* TCP-specific receive */

static int udp_setup_impl(sync_client_t *client);
static int udp_connect_impl(sync_client_t *client, const char *host, int port);
static int udp_send_impl(sync_client_t *client, const char *data, size_t len);
static void udp_close_impl(sync_client_t *client);

static int kcp_setup_impl(sync_client_t *client);
static int kcp_connect_impl(sync_client_t *client, const char *host, int port);
static int kcp_send_impl(sync_client_t *client, const char *data, size_t len);
static void kcp_close_impl(sync_client_t *client);

static int tls_setup_impl(sync_client_t *client);
static int tls_connect_impl(sync_client_t *client, const char *host, int port);
static int tls_send_impl(sync_client_t *client, const char *data, size_t len);
static void tls_close_impl(sync_client_t *client);

static int pipe_setup_impl(sync_client_t *client);
static int pipe_connect_impl(sync_client_t *client, const char *host, int port);
static int pipe_send_impl(sync_client_t *client, const char *data, size_t len);
static void pipe_close_impl(sync_client_t *client);

static int ws_setup_impl(sync_client_t *client);
static int ws_connect_impl(sync_client_t *client, const char *host, int port);
static int ws_send_impl(sync_client_t *client, const char *data, size_t len);
static int ws_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt);
static void ws_close_impl(sync_client_t *client);

/* ========================================================================
 * Send implementations (single buffer) for each protocol
 * ======================================================================== */

static int tcp_send_impl(sync_client_t *client, const char *data, size_t len) {
  if (!client->proto.tcp.client || !client->proto.tcp.connected) {
    return -1;
  }

  turbo_arena_buffer_t *buffer = turbo_tcp_get_send_buffer(client->proto.tcp.client, len);
  if (!buffer) {
    return UV_ENOMEM;
  }

  memcpy(buffer->data, data, len);
  turbo_arena_buffer_set_used(buffer, len);
  int rc = turbo_tcp_queue_buffer(client->proto.tcp.client, buffer, len);
  turbo_arena_buffer_unref(buffer);

  if (rc == 0) {
    rc = turbo_tcp_flush(client->proto.tcp.client);
  }
  return rc;
}

static int udp_send_impl(sync_client_t *client, const char *data, size_t len) {
  if (!client->proto.udp.connected && client->proto.udp.remote_addr_len == 0) {
    return -1;
  }

  /* UDP requires storing data for callback cleanup */
  client->pending_send_data = (char *)data;
  client->pending_send_len = len;

  uv_buf_t buf = uv_buf_init((char *)data, (unsigned int)len);
  client->proto.udp.send_req.data = client;

  int rc = uv_udp_send(
      &client->proto.udp.send_req, &client->proto.udp.handle, &buf, 1,
      client->proto.udp.connected ? NULL : (const struct sockaddr *)&client->proto.udp.remote_addr,
      udp_send_cb);

  if (rc != 0) {
    /* Clear on error - callback won't be called */
    client->pending_send_data = NULL;
    client->pending_send_len = 0;
  }

  return rc;
}

static int kcp_send_impl(sync_client_t *client, const char *data, size_t len) {
  if (!client->proto.kcp.initialized) {
    return -1;
  }

  turbo_arena_buffer_t *buffer = turbo_arena_wrap_external((void *)data, len, NULL, NULL);
  if (!buffer) {
    return UV_ENOMEM;
  }

  int rc = turbo_kcp_client_send_buffer(&client->proto.kcp.client, buffer, len);
  turbo_arena_buffer_unref(buffer);
  return rc;
}

static int tls_send_impl(sync_client_t *client, const char *data, size_t len) {
  if (!client->proto.tls.client || !client->proto.tls.client_created ||
      !client->proto.tls.connected) {
    return -1;
  }

  turbo_arena_buffer_t *buffer = turbo_tls_get_send_buffer(client->proto.tls.client, len);
  if (!buffer) {
    return UV_ENOMEM;
  }

  memcpy(buffer->data, data, len);
  turbo_arena_buffer_set_used(buffer, len);
  int rc = turbo_tls_send_buffer(client->proto.tls.client, buffer, len);
  turbo_arena_buffer_unref(buffer);
  return rc;
}

static int pipe_send_impl(sync_client_t *client, const char *data, size_t len) {
  if (!client->proto.pipe.client) {
    return -1;
  }

  turbo_arena_buffer_t *buffer = turbo_pipe_get_send_buffer(client->proto.pipe.client, len);
  if (!buffer) {
    return UV_ENOMEM;
  }

  memcpy(buffer->data, data, len);
  turbo_arena_buffer_set_used(buffer, len);
  int rc = turbo_pipe_send_buffer(client->proto.pipe.client, buffer, len);
  turbo_arena_buffer_unref(buffer);

  if (rc == 0) {
    rc = turbo_pipe_flush(client->proto.pipe.client);
  }
  return rc;
}

/* ========================================================================
 * Setup implementations - initialize protocol-specific state
 * ======================================================================== */

static int tcp_setup_impl(sync_client_t *client) {
  client->proto.tcp.client = turbo_tcp_client_create(&client->loop);
  if (!client->proto.tcp.client)
    return SYNC_CLIENT_STATUS_ALLOC_FAILED;
  client->proto.tcp.client->user_data = client;
  client->proto.tcp.client->on_close = tcp_client_close_cb;
  client->proto.tcp.connected = 0;
  return SYNC_CLIENT_STATUS_OK;
}

static int udp_setup_impl(sync_client_t *client) {
  int rc = uv_udp_init(&client->loop, &client->proto.udp.handle);
  if (rc != 0)
    return SYNC_CLIENT_STATUS_IO_ERROR;
  client->proto.udp.handle.data = client;
  client->proto.udp.connected = 0;
  client->proto.udp.recv_active = 0;
  client->proto.udp.remote_addr_len = 0;
  return SYNC_CLIENT_STATUS_OK;
}

static int kcp_setup_impl(sync_client_t *client) {
  int rc = turbo_kcp_client_init(&client->proto.kcp.client, &client->loop);
  if (rc != 0)
    return SYNC_CLIENT_STATUS_IO_ERROR;
  client->proto.kcp.initialized = 1;
  return SYNC_CLIENT_STATUS_OK;
}

static int tls_setup_impl(sync_client_t *client) {
  client->proto.tls.client = NULL;
  client->proto.tls.context_initialized = 0;
  client->proto.tls.client_created = 0;
  client->proto.tls.handshake_pending = 0;
  return SYNC_CLIENT_STATUS_OK;
}

static int pipe_setup_impl(sync_client_t *client) {
  client->proto.pipe.client = NULL;
  return SYNC_CLIENT_STATUS_OK;
}

static int ws_setup_impl(sync_client_t *client) {
  client->proto.ws.client = NULL;
  client->proto.ws.connected = 0;
  client->proto.ws.path_owned = NULL;
  client->proto.ws.origin_owned = NULL;
  client->proto.ws.subprotocols_owned = NULL;
  client->proto.ws.subprotocols_owned_count = 0;
  client->proto.ws.tls_context_initialized = 0;
  memset(&client->proto.ws.config, 0, sizeof(client->proto.ws.config));
  return SYNC_CLIENT_STATUS_OK;
}

/* ========================================================================
 * Close implementations - cleanup protocol resources
 * ======================================================================== */

static void tcp_close_impl(sync_client_t *client) {
  if (client->proto.tcp.client) {
    turbo_tcp_client_t *tcp = client->proto.tcp.client;
    /* Note: Don't set client->proto.tcp.client = NULL here.
     * Let tcp_client_close_cb() do it after close completes.
     * This ensures tcp pointer remains valid during async close. */
    client->proto.tcp.connected = 0;
    turbo_tcp_client_close(tcp);
  }
}

static int tcp_receive_impl(sync_client_t *client) {
  uv_mutex_lock(&client->mutex);

  /* Check if data already available */
  if (client->response && client->response_len > 0) {
    result_reset(client);
    client->done = 1;
    client->awaiting_receive = 0;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return 0;
  }

  /* Mark as awaiting receive */
  client->awaiting_receive = 1;
  uv_mutex_unlock(&client->mutex);

  /* Verify TCP connection state */
  if (!client->proto.tcp.client || !client->proto.tcp.connected) {
    uv_mutex_lock(&client->mutex);
    result_set_error(client, SYNC_CLIENT_STATUS_NOT_READY, "tcp client not connected");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return -1;
  }

  return 0;
}

static void udp_close_impl(sync_client_t *client) {
  uv_udp_recv_stop(&client->proto.udp.handle);
  if (!uv_is_closing((uv_handle_t *)&client->proto.udp.handle))
    uv_close((uv_handle_t *)&client->proto.udp.handle, NULL);
  client->proto.udp.connected = 0;
  client->proto.udp.recv_active = 0;
  client->proto.udp.remote_addr_len = 0;
}

static void kcp_close_impl(sync_client_t *client) {
  if (client->proto.kcp.initialized) {
    turbo_kcp_client_close(&client->proto.kcp.client);
    client->proto.kcp.initialized = 0;
  }
}

static void tls_close_impl(sync_client_t *client) {
  if (client->proto.tls.client) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    client->proto.tls.handshake_pending = 0;
    client->proto.tls.connected = 0;
  }
  if (client->proto.tls.context_initialized) {
    turbo_tls_context_destroy(&client->proto.tls.context);
    client->proto.tls.context_initialized = 0;
  }
}

static void pipe_close_impl(sync_client_t *client) {
  if (client->proto.pipe.client) {
    turbo_pipe_client_close(client->proto.pipe.client);
    client->proto.pipe.client = NULL;
  }
}

static void ws_close_impl(sync_client_t *client) {
  if (client->proto.ws.client) {
    turbo_websocket_client_close(client->proto.ws.client, 1000, NULL);
    turbo_websocket_client_destroy(client->proto.ws.client);
    client->proto.ws.client = NULL;
    client->proto.ws.connected = 0;
  }
}

/* Free owned WebSocket config strings (caller must hold mutex) */
static void ws_free_config_strings(sync_client_t *client) {
  if (!client)
    return;

  if (client->proto.ws.path_owned) {
    free(client->proto.ws.path_owned);
    client->proto.ws.path_owned = NULL;
  }

  if (client->proto.ws.origin_owned) {
    free(client->proto.ws.origin_owned);
    client->proto.ws.origin_owned = NULL;
  }

  if (client->proto.ws.subprotocols_owned) {
    for (int i = 0; i < client->proto.ws.subprotocols_owned_count; i++) {
      free(client->proto.ws.subprotocols_owned[i]);
    }
    free(client->proto.ws.subprotocols_owned);
    client->proto.ws.subprotocols_owned = NULL;
    client->proto.ws.subprotocols_owned_count = 0;
  }

  client->proto.ws.config.path = NULL;
  client->proto.ws.config.origin = NULL;
  client->proto.ws.config.subprotocols = NULL;
  client->proto.ws.config.subprotocol_count = 0;
}

/* ========================================================================
 * Connect implementations - establish connection to remote host
 *
 * NOTE: These are complex and keep similar structure to original switch cases.
 * They still interact directly with client state for error handling.
 * A future refactoring could make these return error codes instead.
 * ======================================================================== */

static int tcp_connect_impl(sync_client_t *client, const char *host, int port) {
  /* Update statistics */
  uv_mutex_lock(&client->mutex);
  client->stats.connection_attempts++;
  uv_mutex_unlock(&client->mutex);

  if (!client->proto.tcp.client) {
    client->proto.tcp.client = turbo_tcp_client_create(&client->loop);
    if (!client->proto.tcp.client) {
      uv_mutex_lock(&client->mutex);
      client->stats.connection_failures++;
      result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to allocate tcp client");
      client->done = 1;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
      return -1;
    }
    client->proto.tcp.client->user_data = client;
  }
  client->proto.tcp.connected = 0;

  int rc = turbo_tcp_client_connect(client->proto.tcp.client, host, (unsigned short)port,
                                    tcp_client_recv_cb, tcp_client_connect_cb, tcp_client_close_cb);
  if (rc != 0) {
    uv_mutex_lock(&client->mutex);
    client->stats.connection_failures++;
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return rc;
  }
  return 0;
}

static int udp_connect_impl(sync_client_t *client, const char *host, int port) {
  struct sockaddr_storage addr;
  int addr_len = 0;
  int rc = turbo_dns_resolve(NULL, host, port, &addr, &addr_len);
  if (rc != 0) {
    uv_mutex_lock(&client->mutex);
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return rc;
  }
  rc = uv_udp_connect(&client->proto.udp.handle, (const struct sockaddr *)&addr);
  if (rc != 0) {
    uv_mutex_lock(&client->mutex);
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return rc;
  }
  memcpy(&client->proto.udp.remote_addr, &addr, (size_t)addr_len);
  client->proto.udp.remote_addr_len = addr_len;
  client->proto.udp.connected = 1;
  if (!client->proto.udp.recv_active) {
    rc = uv_udp_recv_start(&client->proto.udp.handle, udp_alloc_cb, udp_read_cb);
    if (rc != 0) {
      uv_mutex_lock(&client->mutex);
      result_set_uv_error(client, rc);
      client->done = 1;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
      return rc;
    }
    client->proto.udp.recv_active = 1;
  }
  uv_mutex_lock(&client->mutex);
  client->state = SYNC_CLIENT_STATE_CONNECTED;
  result_reset(client);
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
  return 0;
}

static int kcp_connect_impl(sync_client_t *client, const char *host, int port) {
  if (!client->proto.kcp.initialized) {
    uv_mutex_lock(&client->mutex);
    result_set_transport_error(client, "kcp client not initialized");
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return -1;
  }
  int rc = turbo_kcp_client_connect(&client->proto.kcp.client, host, (unsigned short)port,
                                    kcp_connect_cb, kcp_recv_cb);
  if (rc != 0) {
    uv_mutex_lock(&client->mutex);
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
  }
  return rc;
}

static int tls_connect_impl(sync_client_t *client, const char *host, int port) {
  if (!client->proto.tls.context_initialized) {
    int rc = turbo_tls_context_init(&client->proto.tls.context, TURBO_TLS_CONTEXT_LIB_INIT);
    if (rc != 0) {
      uv_mutex_lock(&client->mutex);
      result_set_uv_error(client, rc);
      client->done = 1;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
      return rc;
    }
    client->proto.tls.context_initialized = 1;
    turbo_tls_context_set_verify_flags(&client->proto.tls.context, TURBO_TLS_VERIFY_NONE);
  }

  if (client->proto.tls.client_created && client->proto.tls.client) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    client->proto.tls.handshake_pending = 0;
    client->proto.tls.connected = 0;
  }

  client->proto.tls.client = turbo_tls_client_create(&client->loop, &client->proto.tls.context);
  if (!client->proto.tls.client) {
    uv_mutex_lock(&client->mutex);
    result_set_transport_error(client, "failed to allocate tls client");
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return -1;
  }
  client->proto.tls.client_created = 1;
  client->proto.tls.client->user_data = client;
  client->proto.tls.client->handshake_done_cb = tls_handshake_cb;

  if (host && host[0] != '\0')
    turbo_tls_client_set_hostname(client->proto.tls.client, host, strlen(host));

  /* Resolve hostname to IP address first */
  struct sockaddr_storage addr;
  int addr_len = 0;
  int rc = turbo_dns_resolve(NULL, host, port, &addr, &addr_len);
  if (rc != 0) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    uv_mutex_lock(&client->mutex);
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return rc;
  }

  /* Extract IP address string from resolved address */
  char ip_str[INET6_ADDRSTRLEN];
  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
    inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
  } else if (addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
    inet_ntop(AF_INET6, &addr6->sin6_addr, ip_str, sizeof(ip_str));
  } else {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    uv_mutex_lock(&client->mutex);
    result_set_transport_error(client, "unsupported address family");
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return -1;
  }

  /* Connect using resolved IP address */
  rc = turbo_tls_client_connect(client->proto.tls.client, ip_str, (unsigned short)port, tls_recv_cb,
                                tls_connect_cb, tls_close_cb);
  if (rc != 0) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    uv_mutex_lock(&client->mutex);
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return rc;
  }
  client->proto.tls.handshake_pending = 1;
  return 0;
}

static int pipe_connect_impl(sync_client_t *client, const char *host, int port) {
  (void)port; /* PIPE doesn't use port */

  if (!client->proto.pipe.client) {
    client->proto.pipe.client = turbo_pipe_client_create(&client->loop);
    if (!client->proto.pipe.client) {
      uv_mutex_lock(&client->mutex);
      result_set_transport_error(client, "failed to allocate pipe client");
      client->done = 1;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
      return -1;
    }
  }

  int rc = turbo_pipe_client_connect(client->proto.pipe.client, host, pipe_recv_cb, pipe_connect_cb,
                                     pipe_close_cb);
  if (rc != 0) {
    uv_mutex_lock(&client->mutex);
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
  }
  return rc;
}

/* ========================================================================
 * WebSocket callbacks
 * ======================================================================== */

static void ws_connect_cb(void *client_ptr, int status, void *peer) {
  (void)peer;
  turbo_websocket_client_t *ws_client = (turbo_websocket_client_t *)client_ptr;
  sync_client_t *client = (sync_client_t *)ws_client->user_data;
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  if (status == 0) {
    result_reset(client);
    client->proto.ws.connected = 1;
    client->state = SYNC_CLIENT_STATE_CONNECTED;
  } else {
    result_set_uv_error(client, status);
    client->stats.connection_failures++;
  }
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static int ws_recv_cb(void *client_ptr, const turbo_arena_slice_t *data, void *peer) {
  (void)peer;
  TLOG_DEBUG("ws_recv_cb: client_ptr={} data={}", client_ptr, (void *)data);
  turbo_websocket_client_t *ws_client = (turbo_websocket_client_t *)client_ptr;
  sync_client_t *client = (sync_client_t *)ws_client->user_data;
  TLOG_DEBUG("ws_recv_cb: client={} data_len={}", (void *)client, data ? data->length : 0);
  if (!client || !data || data->length == 0)
    return 0;

  char *copy = (char *)malloc(data->length);
  if (!copy) {
    uv_mutex_lock(&client->mutex);
    result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "allocation failure");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return 0;
  }
  memcpy(copy, data->data, data->length);

  uv_mutex_lock(&client->mutex);
  if (client->response)
    free(client->response);
  client->response = copy;
  client->response_len = data->length;

  client->stats.bytes_received += data->length;
  client->stats.messages_received++;

  result_reset(client);
  if (client->awaiting_receive) {
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
  return 0;
}

static void ws_close_cb(void *client_ptr) {
  turbo_websocket_client_t *ws_client = (turbo_websocket_client_t *)client_ptr;
  sync_client_t *client = (sync_client_t *)ws_client->user_data;
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  client->proto.ws.connected = 0;
  if (client->awaiting_receive) {
    result_set_transport_error(client, "websocket closed");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
}

static int ws_connect_impl(sync_client_t *client, const char *host, int port) {
  TLOG_DEBUG("ws_connect_impl: start host={} port={}", host, port);
  uv_mutex_lock(&client->mutex);
  client->stats.connection_attempts++;
  uv_mutex_unlock(&client->mutex);

  /* Create WebSocket config */
  turbo_websocket_config_t ws_config = {0};
  ws_config.path = client->proto.ws.config.path ? client->proto.ws.config.path : "/";
  ws_config.origin = client->proto.ws.config.origin;
  ws_config.subprotocols = client->proto.ws.config.subprotocols;
  ws_config.subprotocol_count = client->proto.ws.config.subprotocol_count;
  ws_config.host = host;
  TLOG_DEBUG("ws_connect_impl: config path={} subprotocol_count={} use_tls={}", ws_config.path,
             ws_config.subprotocol_count, client->proto.ws.config.use_tls);

  /* Create WebSocket client */
  TLOG_DEBUG("ws_connect_impl: calling turbo_websocket_client_create");
  client->proto.ws.client =
      turbo_websocket_client_create(&client->loop, client->proto.ws.config.use_tls, &ws_config);

  if (!client->proto.ws.client) {
    TLOG_DEBUG("ws_connect_impl: turbo_websocket_client_create failed");
    uv_mutex_lock(&client->mutex);
    client->stats.connection_failures++;
    result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to create websocket client");
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return -1;
  }
  TLOG_DEBUG("ws_connect_impl: turbo_websocket_client_create succeeded");

  client->proto.ws.client->user_data = client;

  /* Configure TLS context for wss:// */
  if (client->proto.ws.config.use_tls) {
    if (!client->proto.ws.tls_context_initialized) {
      int tls_rc =
          turbo_tls_context_init(&client->proto.ws.tls_context, TURBO_TLS_CONTEXT_LIB_INIT);
      if (tls_rc != 0) {
        turbo_websocket_client_destroy(client->proto.ws.client);
        client->proto.ws.client = NULL;
        uv_mutex_lock(&client->mutex);
        client->stats.connection_failures++;
        result_set_uv_error(client, tls_rc);
        client->done = 1;
        uv_cond_signal(&client->cond);
        uv_mutex_unlock(&client->mutex);
        return tls_rc;
      }
      /* For now, disable peer verification to allow public brokers without CA config */
      turbo_tls_context_set_verify_flags(&client->proto.ws.tls_context, TURBO_TLS_VERIFY_NONE);
      client->proto.ws.tls_context_initialized = 1;
    }
    turbo_websocket_client_set_tls_context(client->proto.ws.client, &client->proto.ws.tls_context);
  }

  /* Set callbacks */
  turbo_websocket_client_set_callbacks(client->proto.ws.client, ws_recv_cb, ws_connect_cb,
                                       ws_close_cb);

  /* Connect */
  TLOG_DEBUG("ws_connect_impl: calling turbo_websocket_client_connect");
  int rc = turbo_websocket_client_connect(client->proto.ws.client, host, port);
  TLOG_DEBUG("ws_connect_impl: turbo_websocket_client_connect returned rc={}", rc);
  if (rc != 0) {
    turbo_websocket_client_destroy(client->proto.ws.client);
    client->proto.ws.client = NULL;
    uv_mutex_lock(&client->mutex);
    client->stats.connection_failures++;
    result_set_uv_error(client, rc);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return rc;
  }

  return 0;
}

static int ws_send_impl(sync_client_t *client, const char *data, size_t len) {
  TLOG_DEBUG("ws_send_impl: ENTER len={}", len);
  TLOG_DEBUG("ws_send_impl: client={} proto.ws.client={} connected={}", (void *)client,
             (void *)client->proto.ws.client, client->proto.ws.connected);
  if (!client->proto.ws.client || !client->proto.ws.connected) {
    TLOG_DEBUG("ws_send_impl: NOT READY");
    return -1;
  }
  TLOG_DEBUG("ws_send_impl: calling turbo_websocket_client_send");
  int rc = turbo_websocket_client_send(client->proto.ws.client, data, len);
  TLOG_DEBUG("ws_send_impl: turbo_websocket_client_send returned {}", rc);
  return rc;
}

static int ws_sendv_impl(sync_client_t *client, sync_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.ws.client || !client->proto.ws.connected) {
    return -1;
  }

  /* WebSocket doesn't have native sendv, so we concatenate and send */
  size_t total_len = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    total_len += iov[i].len;
  }

  if (total_len == 0)
    return 0;

  char *buffer = (char *)malloc(total_len);
  if (!buffer)
    return UV_ENOMEM;

  size_t offset = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      memcpy(buffer + offset, iov[i].data, iov[i].len);
      offset += iov[i].len;
    }
  }

  int rc = turbo_websocket_client_send(client->proto.ws.client, buffer, total_len);
  free(buffer);
  return rc;
}

/* ========================================================================
 * Refactored handle_sendv_command - 357 lines -> ~60 lines
 * ======================================================================== */

static void handle_sendv_command(sync_client_t *client, sync_client_command_t *cmd) {
  sync_client_iovec_t *iov = cmd->payload.sendv.iov;
  size_t iovcnt = cmd->payload.sendv.iovcnt;

  /* Early return for empty IOV */
  if (iovcnt == 0 || !iov) {
    signal_done_with_status(client, 0, SYNC_CLIENT_STATUS_OK, NULL);
    return;
  }

  /* Transfer ownership to avoid double-free */
  cmd->payload.sendv.iov = NULL;

  /* Calculate total bytes for stats */
  size_t total_bytes = calculate_iov_total_bytes(iov, iovcnt);

  /* Dispatch via vtable - NO SWITCH STATEMENT! This is "good taste". */
  int rc;
  if (client->ops && client->ops->sendv) {
    rc = client->ops->sendv(client, iov, iovcnt);
  } else {
    rc = -1; /* Not ready */
  }

  /* Special case: UDP cleanup happens in callback */
  if (client->transport == SYNC_CLIENT_TRANSPORT_UDP) {
    if (rc == 0) {
      /* Stats already updated in udp_sendv_impl */
      return;
    }
    cleanup_iov_buffers(iov, iovcnt);
    signal_done_with_status(client, rc, SYNC_CLIENT_STATUS_NOT_READY, "udp send failed");
    return;
  }

  /* Cleanup IOV buffers */
  cleanup_iov_buffers(iov, iovcnt);

  /* Update stats and signal completion */
  uv_mutex_lock(&client->mutex);
  if (rc == 0) {
    result_reset(client);
    update_send_stats_locked(client, total_bytes, iovcnt);
  } else {
    /* rc == -1 means "not connected/not ready" from transport layer
     * Other negative values are libuv error codes */
    if (rc == -1) {
      result_set_error(client, SYNC_CLIENT_STATUS_NOT_READY, "transport not ready");
    } else if (rc < 0) {
      result_set_uv_error(client, rc);
    }
    update_error_stats_locked(client);
  }
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static void handle_receive_command(sync_client_t *client) {
  if (!client->ops) {
    signal_done_with_status(client, -1, SYNC_CLIENT_STATUS_NOT_READY, "client not connected");
    return;
  }

  /* Dispatch via vtable for protocol-specific receive handling (TCP only) */
  if (client->ops && client->ops->receive) {
    client->ops->receive(client);
    return;
  }

  /* Protocols without specific receive handling - just mark awaiting */
  switch (client->transport) {
  case SYNC_CLIENT_TRANSPORT_TCP:
    /* Already handled via vtable above */
    break;
  case SYNC_CLIENT_TRANSPORT_TLS:
    /* TLS: Check if data already available, otherwise wait for callback */
    uv_mutex_lock(&client->mutex);
    if (client->response && client->response_len > 0) {
      result_reset(client);
      client->done = 1;
      client->awaiting_receive = 0;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
      return;
    }
    client->awaiting_receive = 1;
    uv_mutex_unlock(&client->mutex);

    if (!client->proto.tls.client || !client->proto.tls.connected) {
      uv_mutex_lock(&client->mutex);
      result_set_error(client, SYNC_CLIENT_STATUS_NOT_READY, "tls client not connected");
      client->awaiting_receive = 0;
      client->done = 1;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
    }
    break;
  case SYNC_CLIENT_TRANSPORT_UDP:
  case SYNC_CLIENT_TRANSPORT_KCP:
  case SYNC_CLIENT_TRANSPORT_PIPE:
  case SYNC_CLIENT_TRANSPORT_WEBSOCKET:
    client->awaiting_receive = 1;
    break;
  }
}

static void handle_stop_command(sync_client_t *client) {
  uv_mutex_lock(&client->mutex);
  result_set_error(client, SYNC_CLIENT_STATUS_SHUTTING_DOWN, NULL);
  client->done = 1;
  client->awaiting_receive = 0;
  sync_client_command_t *pending = client->command_head;
  client->command_head = NULL;
  client->command_tail = NULL;
  uv_cond_broadcast(&client->cond);
  uv_mutex_unlock(&client->mutex);

  while (pending) {
    sync_client_command_t *next = pending->next;
    free_command(pending);
    pending = next;
  }

  /* Note: transport_close() is called in loop_thread_main() after loop exits.
   * Calling it here would set client->proto.tcp.client = NULL before the loop
   * has a chance to process the close callback, causing the server to hang
   * waiting for connection close that never completes. */

  if (!uv_is_closing((uv_handle_t *)&client->command_async))
    uv_close((uv_handle_t *)&client->command_async, NULL);

  uv_stop(&client->loop);
}
static void command_async_cb(uv_async_t *handle) {
  sync_client_t *client = handle->data;

  for (;;) {
    sync_client_command_t *cmd = NULL;

    uv_mutex_lock(&client->mutex);
    if (client->command_head) {
      cmd = client->command_head;
      client->command_head = cmd->next;
      if (!client->command_head)
        client->command_tail = NULL;
    }
    int shutting_down = client->shutting_down;
    UNUSED(shutting_down);
    uv_mutex_unlock(&client->mutex);

    if (!cmd) {
      break;
    }

    switch (cmd->type) {
    case COMMAND_CONNECT:
      TLOG_DEBUG("command_async_cb: COMMAND_CONNECT");
      handle_connect_command(client, cmd);
      break;
    case COMMAND_SEND:
      TLOG_DEBUG("command_async_cb: COMMAND_SEND len={}", cmd->payload.send.len);
      handle_send_command(client, cmd);
      break;
    case COMMAND_SENDV:
      TLOG_DEBUG("command_async_cb: COMMAND_SENDV iovcnt={}", cmd->payload.sendv.iovcnt);
      handle_sendv_command(client, cmd);
      break;
    case COMMAND_RECEIVE:
      TLOG_DEBUG("command_async_cb: COMMAND_RECEIVE");
      handle_receive_command(client);
      break;
    case COMMAND_STOP:
      TLOG_DEBUG("command_async_cb: COMMAND_STOP");
      handle_stop_command(client);
      break;
    default:
      TLOG_DEBUG("command_async_cb: UNKNOWN type={}", (int)cmd->type);
      break;
    }

    free_command(cmd);
  }
}

static void tcp_client_connect_cb(turbo_tcp_client_t *tcp_client, int status, void *peer) {
  (void)peer;
  if (!tcp_client)
    return;
  if (status != 0) {
    sync_client_t *client = (sync_client_t *)tcp_client->user_data;
    if (!client)
      return;
    uv_mutex_lock(&client->mutex);
    client->stats.connection_failures++;
    result_set_uv_error(client, status);
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return;
  }
  sync_client_t *client = (sync_client_t *)tcp_client->user_data;
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  client->proto.tcp.connected = 1;
  client->state = SYNC_CLIENT_STATE_CONNECTED;
  result_reset(client);
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static int tcp_client_recv_cb(turbo_tcp_client_t *tcp_client, const turbo_arena_slice_t *data,
                              void *peer) {
  (void)peer;
  if (!tcp_client || !data || data->length == 0)
    return 0;
  sync_client_t *client = (sync_client_t *)tcp_client->user_data;
  if (!client)
    return 0;

  char *copy = (char *)malloc(data->length);
  if (!copy) {
    uv_mutex_lock(&client->mutex);
    result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED,
                     "failed to allocate tcp receive buffer");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return 0;
  }
  memcpy(copy, data->data, data->length);

  uv_mutex_lock(&client->mutex);
  if (client->response)
    free(client->response);
  client->response = copy;
  client->response_len = data->length;

  /* Update statistics */
  client->stats.bytes_received += data->length;
  client->stats.messages_received++;

  result_reset(client);
  if (client->awaiting_receive) {
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
  return 0;
}

static void tcp_client_close_cb(void *handle) {
  turbo_tcp_client_t *tcp_client = (turbo_tcp_client_t *)handle;
  if (!tcp_client)
    return;
  sync_client_t *client = (sync_client_t *)tcp_client->user_data;
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  int was_connected = client->proto.tcp.connected;
  client->proto.tcp.client = NULL;
  client->proto.tcp.connected = 0;
  if (!client->done) {
    /* Connection closed before operation completed - this is a failure */
    if (!was_connected) {
      /* Never connected = connection failure */
      client->stats.connection_failures++;
    }
    result_set_error(client, SYNC_CLIENT_STATUS_TRANSPORT_ERROR, "tcp connection closed");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
}

static void udp_send_cb(uv_udp_send_t *req, int status) {
  sync_client_t *client = req->data;

  /* Check if this is a scatter-gather send (iov stored in pending_send_data) */
  if (client->pending_send_data && client->pending_send_len > 1) {
    /* This is scatter-gather - cleanup IOV array */
    sync_client_iovec_t *iov = (sync_client_iovec_t *)client->pending_send_data;
    size_t iovcnt = client->pending_send_len;

    for (size_t i = 0; i < iovcnt; i++) {
      free((void *)iov[i].data);
    }
    free(iov);

    client->pending_send_data = NULL;
    client->pending_send_len = 0;
  } else if (client->pending_send_data) {
    /* Regular send - single buffer */
    free(client->pending_send_data);
    client->pending_send_data = NULL;
    client->pending_send_len = 0;
  }

  uv_mutex_lock(&client->mutex);
  if (status < 0)
    result_set_uv_error(client, status);
  else
    result_reset(client);
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  UNUSED(handle);
  if (suggested_size == 0)
    suggested_size = 4096;
  buf->base = (char *)malloc(suggested_size);
  if (!buf->base) {
    buf->len = 0;
    return;
  }
  buf->len = (unsigned int)suggested_size;
}

static void udp_read_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
  UNUSED(addr);
  UNUSED(flags);
  sync_client_t *client = handle->data;

  if (!buf->base)
    return;

  if (nread <= 0) {
    if (buf->base)
      free(buf->base);
    if (nread < 0) {
      uv_mutex_lock(&client->mutex);
      result_set_uv_error(client, (int)nread);
      client->awaiting_receive = 0;
      client->done = 1;
      uv_cond_signal(&client->cond);
      uv_mutex_unlock(&client->mutex);
    }
    return;
  }

  char *data = buf->base;
  size_t available = buf->len;
  if ((size_t)nread < available) {
    char *shrunk = (char *)realloc(data, (size_t)nread);
    if (shrunk)
      data = shrunk;
  }

  uv_mutex_lock(&client->mutex);
  if (client->response)
    free(client->response);
  client->response = data;
  client->response_len = (size_t)nread;

  /* Update statistics */
  client->stats.bytes_received += (size_t)nread;
  client->stats.messages_received++;

  result_reset(client);
  client->awaiting_receive = 0;
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static void kcp_connect_cb(turbo_kcp_client_t *kcp_client, int status, void *peer) {
  (void)peer;
  sync_client_t *client = CONTAINER_OF(kcp_client, sync_client_t, proto.kcp.client);
  uv_mutex_lock(&client->mutex);
  if (status == 0) {
    result_reset(client);
    client->state = SYNC_CLIENT_STATE_CONNECTED;
  } else {
    result_set_uv_error(client, status);
  }
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static int kcp_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)handle;
  sync_client_t *client = CONTAINER_OF(kcp_client, sync_client_t, proto.kcp.client);

  if (!client || !data || data->length == 0)
    return 0;

  char *copy = (char *)malloc(data->length);
  if (!copy) {
    uv_mutex_lock(&client->mutex);
    result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "allocation failure");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return 0;
  }
  memcpy(copy, data->data, data->length);

  uv_mutex_lock(&client->mutex);
  if (client->response)
    free(client->response);
  client->response = copy;
  client->response_len = data->length;

  /* Update statistics */
  client->stats.bytes_received += data->length;
  client->stats.messages_received++;

  result_reset(client);
  client->awaiting_receive = 0;
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
  return 0;
}

static sync_client_t *client_from_tls_impl(turbo_tls_client_t *tls_client) {
  if (!tls_client)
    return NULL;
  return (sync_client_t *)tls_client->user_data;
}

static void tls_connect_cb(turbo_tls_client_t *tls_client, int status, void *peer) {
  (void)peer;
  sync_client_t *client = client_from_tls_impl(tls_client);
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  if (status == 0) {
    result_reset(client);
    client->proto.tls.handshake_pending = 0;
    client->proto.tls.connected = 1;
    client->state = SYNC_CLIENT_STATE_CONNECTED;
    client->done = 1;
    uv_cond_signal(&client->cond);
  } else {
    result_set_uv_error(client, status);
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
}

static void tls_close_cb(void *handle) {
  turbo_tls_client_t *tls_client = (turbo_tls_client_t *)handle;
  sync_client_t *client = client_from_tls_impl(tls_client);
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  if (client->awaiting_receive) {
    result_set_transport_error(client, "tls connection closed");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);

  client->proto.tls.client = NULL;
  client->proto.tls.client_created = 0;
  client->proto.tls.handshake_pending = 0;
  client->proto.tls.connected = 0;
}

static void tls_handshake_cb(turbo_tls_client_t *tls_client, int status) {
  sync_client_t *client = client_from_tls_impl(tls_client);
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  if (status == 0) {
    result_reset(client);
    client->proto.tls.handshake_pending = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  } else {
    char buffer[64];
    static const char FMT_TLS_FAIL[32] = "tls handshake failed (%d)";
    stbsp_snprintf(buffer, (int)sizeof(buffer), FMT_TLS_FAIL, status);
    result_set_transport_error(client, buffer);
    client->uv_status = status;
    client->proto.tls.handshake_pending = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
}

static int tls_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_tls_client_t *tls_client = (turbo_tls_client_t *)handle;
  sync_client_t *client = client_from_tls_impl(tls_client);
  if (!client || !data || data->length == 0)
    return 0;

  char *copy = (char *)malloc(data->length);
  if (!copy) {
    uv_mutex_lock(&client->mutex);
    result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "allocation failure");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return 0;
  }
  memcpy(copy, data->data, data->length);

  uv_mutex_lock(&client->mutex);
  if (client->response)
    free(client->response);
  client->response = copy;
  client->response_len = data->length;

  /* Update statistics */
  client->stats.bytes_received += data->length;
  client->stats.messages_received++;

  result_reset(client);
  client->awaiting_receive = 0;
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
  return 0;
}

static int pipe_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_pipe_client_t *pipe_client = (turbo_pipe_client_t *)handle;
  sync_client_t *client = client_from_pipe(pipe_client);
  if (!client || !data || data->length == 0)
    return 0;

  char *copy = (char *)malloc(data->length);
  if (!copy) {
    uv_mutex_lock(&client->mutex);
    result_set_error(client, SYNC_CLIENT_STATUS_ALLOC_FAILED, "allocation failure");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
    uv_mutex_unlock(&client->mutex);
    turbo_arena_slice_release((turbo_arena_slice_t *)data);
    return 0;
  }
  memcpy(copy, data->data, data->length);
  turbo_arena_slice_release((turbo_arena_slice_t *)data);

  uv_mutex_lock(&client->mutex);
  if (client->response)
    free(client->response);
  client->response = copy;
  client->response_len = data->length;

  /* Update statistics */
  client->stats.bytes_received += data->length;
  client->stats.messages_received++;

  result_reset(client);
  client->awaiting_receive = 0;
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
  return 0;
}

static void pipe_connect_cb(turbo_pipe_client_t *pipe_client, int status, void *peer) {
  (void)peer;
  sync_client_t *client = client_from_pipe(pipe_client);
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  if (status == 0) {
    result_reset(client);
    client->state = SYNC_CLIENT_STATE_CONNECTED;
  } else {
    result_set_uv_error(client, status);
  }
  client->done = 1;
  uv_cond_signal(&client->cond);
  uv_mutex_unlock(&client->mutex);
}

static void pipe_close_cb(void *handle) {
  turbo_pipe_client_t *pipe_client = (turbo_pipe_client_t *)handle;
  sync_client_t *client = client_from_pipe(pipe_client);
  if (!client)
    return;
  uv_mutex_lock(&client->mutex);
  if (client->awaiting_receive) {
    result_set_transport_error(client, "pipe closed");
    client->awaiting_receive = 0;
    client->done = 1;
    uv_cond_signal(&client->cond);
  }
  uv_mutex_unlock(&client->mutex);
  client->proto.pipe.client = NULL;
}
static void loop_thread_main(void *arg) {
  sync_client_t *client = (sync_client_t *)arg;
  int rc = uv_loop_init(&client->loop);
  int async_initialized = 0;

  if (rc == 0) {
    rc = uv_async_init(&client->loop, &client->command_async, command_async_cb);
    if (rc == 0) {
      async_initialized = 1;
      client->command_async.data = client;
    }
  }

  /* Transport setup now happens during first connect command */
  if (rc == 0) {
    /* No-op here, setup deferred */
  }

  if (rc != 0) {
    if (async_initialized && !uv_is_closing((uv_handle_t *)&client->command_async)) {
      uv_close((uv_handle_t *)&client->command_async, NULL);
      uv_run(&client->loop, UV_RUN_DEFAULT);
    }
    uv_loop_close(&client->loop);

    uv_mutex_lock(&client->mutex);
    client->init_error = rc;
    client->loop_ready = 1;
    client->loop_running = 0;
    uv_cond_broadcast(&client->cond);
    uv_mutex_unlock(&client->mutex);
    return;
  }

  uv_mutex_lock(&client->mutex);
  result_reset(client);
  client->init_error = 0;
  client->loop_ready = 1;
  client->loop_running = 1;
  uv_cond_broadcast(&client->cond);
  uv_mutex_unlock(&client->mutex);

  uv_run(&client->loop, UV_RUN_DEFAULT);

  uv_mutex_lock(&client->mutex);
  client->loop_running = 0;
  uv_cond_broadcast(&client->cond);
  uv_mutex_unlock(&client->mutex);

  transport_close(client);

  if (!uv_is_closing((uv_handle_t *)&client->command_async)) {
    uv_close((uv_handle_t *)&client->command_async, NULL);
  }

  /* Run loop to completion - process all close callbacks.
   * This ensures TCP FIN/ACK handshake completes before thread exits,
   * preventing server-side hang in async_server_destroy().
   *
   * Keep running until loop has no more active handles.
   * uv_loop_alive() returns non-zero if there are active handles or requests. */
  while (uv_loop_alive(&client->loop)) {
    uv_run(&client->loop, UV_RUN_ONCE);
  }

  uv_loop_close(&client->loop);
}

sync_client_transport_t sync_client_get_transport(const sync_client_t *client) {
  if (!client)
    return SYNC_CLIENT_TRANSPORT_TCP;
  return client->transport;
}

sync_client_t *sync_client_create(void) {
  sync_client_t *client = (sync_client_t *)calloc(1, sizeof(*client));
  if (!client)
    return NULL;

  client->transport = (sync_client_transport_t)0;
  client->ops = NULL;

  /* Initialize fields */
  client->state = SYNC_CLIENT_STATE_DISCONNECTED;
  client->connect_timeout_ms = 0; /* 0 = no timeout */
  client->operation_timeout_ms = 0;
  memset(&client->stats, 0, sizeof(client->stats));

  if (uv_mutex_init(&client->mutex) != 0) {
    free(client);
    return NULL;
  }
  if (uv_cond_init(&client->cond) != 0) {
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  if (client_common_config_acquire() == 0) {
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }
  client->config_acquired = 1;

  if (turbo_dns_init() != 0) {
    client_common_config_release();
    client->config_acquired = 0;
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  client_list_register(client);

  if (uv_thread_create(&client->loop_thread, loop_thread_main, client) != 0) {
    client_list_unregister(client);
    client_common_config_release();
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  uv_mutex_lock(&client->mutex);
  while (!client->loop_ready)
    uv_cond_wait(&client->cond, &client->mutex);
  int init_error = client->init_error;
  uv_mutex_unlock(&client->mutex);

  if (init_error != 0) {
    uv_thread_join(&client->loop_thread);
    client_list_unregister(client);
    client_common_config_release();
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  return client;
}

sync_client_t *sync_client_create_with_transport(sync_client_transport_t transport) {
  sync_client_t *client = sync_client_create();
  if (client) {
    client->transport = transport;
    /* Pre-set ops if created with transport */
    switch (transport) {
    case SYNC_CLIENT_TRANSPORT_TCP:
      client->ops = &tcp_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_UDP:
      client->ops = &udp_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_KCP:
      client->ops = &kcp_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_TLS:
      client->ops = &tls_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_PIPE:
      client->ops = &pipe_ops;
      break;
    case SYNC_CLIENT_TRANSPORT_WEBSOCKET:
      client->ops = &websocket_ops;
      break;
    default:
      break;
    }
  }
  return client;
}

void sync_client_destroy(sync_client_t *client) {
  if (!client)
    return;

  if (client->loop_running) {
    sync_client_command_t *stop_cmd = command_create_stop();
    if (stop_cmd) {
      if (submit_command(client, stop_cmd, 0, 1) != SYNC_CLIENT_STATUS_OK)
        free_command(stop_cmd);
    }
  }

  uv_thread_join(&client->loop_thread);

  client_list_unregister(client);

  turbo_dns_cleanup();

  if (client->config_acquired) {
    client_common_config_release();
    client->config_acquired = 0;
  }

  if (client->pending_send_data) {
    free(client->pending_send_data);
    client->pending_send_data = NULL;
  }
  if (client->response) {
    free(client->response);
    client->response = NULL;
  }

  /* Cleanup WebSocket owned config and TLS context */
  if (client->transport == SYNC_CLIENT_TRANSPORT_WEBSOCKET) {
    uv_mutex_lock(&client->mutex);
    ws_free_config_strings(client);
    if (client->proto.ws.tls_context_initialized) {
      turbo_tls_context_destroy(&client->proto.ws.tls_context);
      client->proto.ws.tls_context_initialized = 0;
    }
    uv_mutex_unlock(&client->mutex);
  }

  uv_cond_destroy(&client->cond);
  uv_mutex_destroy(&client->mutex);
  free(client);
}

sync_client_status_t sync_client_connect(sync_client_t *client, const char *url) {
  if (!client || !url)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Parse URL to determine transport and extract connection details */
  turbo_address_t addr;
  int parse_result = parse_transport_url(url, &addr);
  if (parse_result != 0 || !addr.valid) {
    TLOG_ERROR("Failed to parse connection URL: {}", url);
    return SYNC_CLIENT_STATUS_INVALID_PARAM;
  }

  /* Map URL transport to client transport and validate match */
  sync_client_transport_t url_transport;
  switch (addr.transport) {
  case TURBO_TCP:
    url_transport = SYNC_CLIENT_TRANSPORT_TCP;
    break;
  case TURBO_UDP:
    url_transport = SYNC_CLIENT_TRANSPORT_UDP;
    break;
  case TURBO_KCP:
    url_transport = SYNC_CLIENT_TRANSPORT_KCP;
    break;
  case TURBO_TLS:
    url_transport = SYNC_CLIENT_TRANSPORT_TLS;
    break;
  case TURBO_PIPE:
    url_transport = SYNC_CLIENT_TRANSPORT_PIPE;
    break;
  case TURBO_WEBSOCKET:
    url_transport = SYNC_CLIENT_TRANSPORT_WEBSOCKET;
    break;
  default:
    TLOG_ERROR("Unsupported transport type from URL: {}", (int)addr.transport);
    return SYNC_CLIENT_STATUS_TRANSPORT_ERROR;
  }

  /* For pipes, use path instead of host; for others use host */
  const char *connect_host = (url_transport == SYNC_CLIENT_TRANSPORT_PIPE) ? addr.path : addr.host;

  sync_client_command_t *cmd = command_create_connect(url_transport, connect_host, addr.port);
  if (!cmd)
    return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  sync_client_status_t submit = submit_command(client, cmd, 0, 0);
  if (submit != SYNC_CLIENT_STATUS_OK) {
    free_command(cmd);
    return submit;
  }

  uv_mutex_lock(&client->mutex);

  /* Use timeout if specified */
  if (client->connect_timeout_ms > 0) {
    uint64_t deadline = uv_hrtime() + ((uint64_t)client->connect_timeout_ms * 1000000ULL);
    while (!client->done) {
      uint64_t now = uv_hrtime();
      if (now >= deadline) {
        /* Timeout */
        result_set_error(client, SYNC_CLIENT_STATUS_IO_ERROR, "connect timeout");
        client->done = 1;
        break;
      }
      /* Wait with timeout */
      uint64_t remaining_ns = deadline - now;
      uint64_t remaining_ms = remaining_ns / 1000000ULL;
      if (remaining_ms == 0)
        remaining_ms = 1;

      /* Use timed wait */
      uv_cond_timedwait(&client->cond, &client->mutex, remaining_ms);
    }
  } else {
    /* No timeout specified, wait indefinitely */
    while (!client->done)
      uv_cond_wait(&client->cond, &client->mutex);
  }

  sync_client_status_t result = client->result_code;
  uv_mutex_unlock(&client->mutex);
  return result;
}

sync_client_status_t sync_client_send(sync_client_t *client, const char *data, size_t len) {
  if (!client || (!data && len > 0))
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  sync_client_command_t *cmd = command_create_send(data, len);
  if (!cmd)
    return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  sync_client_status_t submit = submit_command(client, cmd, 0, 0);
  if (submit != SYNC_CLIENT_STATUS_OK) {
    free_command(cmd);
    return submit;
  }

  uv_mutex_lock(&client->mutex);
  while (!client->done)
    uv_cond_wait(&client->cond, &client->mutex);
  sync_client_status_t result = client->result_code;
  uv_mutex_unlock(&client->mutex);
  return result;
}

sync_client_status_t sync_client_receive(sync_client_t *client, char **response, size_t *len) {
  if (!client || !response || !len)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  sync_client_command_t *cmd = command_create_receive();
  if (!cmd)
    return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  /* Don't reset response - we want to check if data already arrived!
   * reset_response=0 prevents race condition where data arrives
   * between send completion and receive call. */
  sync_client_status_t submit = submit_command(client, cmd, 0, 0);
  if (submit != SYNC_CLIENT_STATUS_OK) {
    free_command(cmd);
    *response = NULL;
    *len = 0;
    return submit;
  }

  uv_mutex_lock(&client->mutex);

  /* Use timeout if specified */
  if (client->operation_timeout_ms > 0) {
    uint64_t deadline = uv_hrtime() + ((uint64_t)client->operation_timeout_ms * 1000000ULL);
    while (!client->done) {
      uint64_t now = uv_hrtime();
      if (now >= deadline) {
        /* Timeout */
        result_set_error(client, SYNC_CLIENT_STATUS_IO_ERROR, "receive timeout");
        client->awaiting_receive = 0;
        client->done = 1;
        break;
      }
      /* Wait with timeout */
      uint64_t remaining_ns = deadline - now;
      uint64_t remaining_ms = remaining_ns / 1000000ULL;
      if (remaining_ms == 0)
        remaining_ms = 1;

      /* Use timed wait */
      uv_cond_timedwait(&client->cond, &client->mutex, remaining_ms);
    }
  } else {
    /* No timeout - wait indefinitely */
    while (!client->done)
      uv_cond_wait(&client->cond, &client->mutex);
  }

  sync_client_status_t result = client->result_code;
  if (result == SYNC_CLIENT_STATUS_OK) {
    *response = client->response;
    *len = client->response_len;
    client->response = NULL;
    client->response_len = 0;
  } else {
    *response = NULL;
    *len = 0;
  }
  uv_mutex_unlock(&client->mutex);
  return result;
}
#undef g_client_list_once
#undef g_client_list_lock
#undef g_client_list_initialized
#undef g_client_list_head

/* ============================================================================
 * New API Implementations
 * ============================================================================ */

sync_client_state_t sync_client_get_state(const sync_client_t *client) {
  if (!client)
    return SYNC_CLIENT_STATE_ERROR;
  return client->state;
}

int sync_client_is_connected(const sync_client_t *client) {
  if (!client)
    return 0;
  return client->state == SYNC_CLIENT_STATE_CONNECTED;
}

void sync_client_get_stats(const sync_client_t *client, sync_client_stats_t *stats) {
  if (!client || !stats)
    return;

  /* Copy stats with mutex protection */
  uv_mutex_lock((uv_mutex_t *)&client->mutex);
  memcpy(stats, &client->stats, sizeof(sync_client_stats_t));
  uv_mutex_unlock((uv_mutex_t *)&client->mutex);
}

void sync_client_reset_stats(sync_client_t *client) {
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  memset(&client->stats, 0, sizeof(client->stats));
  uv_mutex_unlock(&client->mutex);
}

sync_client_status_t sync_client_connect_timeout(sync_client_t *client, const char *url,
                                                 int timeout_ms) {
  if (!client)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Store timeout for use in connect operation */
  client->connect_timeout_ms = timeout_ms;

  /* Call regular connect (timeout will be used internally) */
  return sync_client_connect(client, url);
}

sync_client_status_t sync_client_receive_timeout(sync_client_t *client, char **response,
                                                 size_t *len, int timeout_ms) {
  if (!client || !response || !len)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Store timeout for use in receive operation */
  client->operation_timeout_ms = timeout_ms;

  /* Call regular receive (timeout will be used internally) */
  return sync_client_receive(client, response, len);
}

sync_client_status_t sync_client_sendv(sync_client_t *client, const sync_client_iovec_t *iov,
                                       size_t iovcnt) {
  if (!client || !iov || iovcnt == 0)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  sync_client_command_t *cmd = command_create_sendv(iov, iovcnt);
  if (!cmd)
    return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  sync_client_status_t submit = submit_command(client, cmd, 0, 0);
  if (submit != SYNC_CLIENT_STATUS_OK) {
    free_command(cmd);
    return submit;
  }

  uv_mutex_lock(&client->mutex);
  while (!client->done)
    uv_cond_wait(&client->cond, &client->mutex);
  sync_client_status_t result = client->result_code;
  uv_mutex_unlock(&client->mutex);
  return result;
}

sync_client_status_t sync_client_recvv(sync_client_t *client, sync_client_iovec_t *iov,
                                       size_t iovcnt, size_t *bytes_read) {
  if (!client || !iov || iovcnt == 0 || !bytes_read)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  *bytes_read = 0;

  /* Receive data into temporary buffer first with 5 second timeout */
  char *response = NULL;
  size_t response_len = 0;

  sync_client_status_t status = sync_client_receive_timeout(client, &response, &response_len, 5000);
  if (status != SYNC_CLIENT_STATUS_OK) {
    return status;
  }

  if (!response || response_len == 0) {
    return SYNC_CLIENT_STATUS_OK;
  }

  /* Distribute received data across buffers */
  size_t offset = 0;
  for (size_t i = 0; i < iovcnt && offset < response_len; i++) {
    size_t to_copy = iov[i].len;
    if (offset + to_copy > response_len) {
      to_copy = response_len - offset;
    }

    if (to_copy > 0 && iov[i].data) {
      memcpy((void *)iov[i].data, response + offset, to_copy);
      offset += to_copy;
      *bytes_read += to_copy;
    }
  }

  free(response);

  /* Update stats */
  uv_mutex_lock(&client->mutex);
  client->stats.bytes_received += *bytes_read;
  client->stats.messages_received++;
  client->stats.scatter_gather_receives++;
  uv_mutex_unlock(&client->mutex);

  return SYNC_CLIENT_STATUS_OK;
}

sync_client_status_t sync_client_set_tls_config(sync_client_t *client,
                                                const sync_client_tls_config_t *config) {
  if (!client || !config)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  if (client->transport != SYNC_CLIENT_TRANSPORT_TLS)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Note: TLS context will be initialized on first connect */
  /* Store config for later use - we need to extend sync_client_s struct */

  uv_mutex_lock(&client->mutex);

  /* If TLS context is already initialized, configure it now */
  if (client->proto.tls.context_initialized) {
    int rc;

    /* Load client certificate and key if provided */
    if (config->cert_file && config->key_file) {
      /* Read cert file */
      FILE *cert_fp = fopen(config->cert_file, "rb");
      if (!cert_fp) {
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_IO_ERROR;
      }
      fseek(cert_fp, 0, SEEK_END);
      long cert_size = ftell(cert_fp);
      fseek(cert_fp, 0, SEEK_SET);
      char *cert_data = (char *)malloc(cert_size + 1);
      if (!cert_data) {
        fclose(cert_fp);
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_ALLOC_FAILED;
      }
      fread(cert_data, 1, cert_size, cert_fp);
      cert_data[cert_size] = '\0';
      fclose(cert_fp);

      /* Read key file */
      FILE *key_fp = fopen(config->key_file, "rb");
      if (!key_fp) {
        free(cert_data);
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_IO_ERROR;
      }
      fseek(key_fp, 0, SEEK_END);
      long key_size = ftell(key_fp);
      fseek(key_fp, 0, SEEK_SET);
      char *key_data = (char *)malloc(key_size + 1);
      if (!key_data) {
        fclose(key_fp);
        free(cert_data);
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_ALLOC_FAILED;
      }
      fread(key_data, 1, key_size, key_fp);
      key_data[key_size] = '\0';
      fclose(key_fp);

      rc = turbo_tls_context_set_cert(&client->proto.tls.context, cert_data, cert_size);
      if (rc == 0) {
        rc = turbo_tls_context_set_private_key(&client->proto.tls.context, key_data, key_size);
      }

      free(cert_data);
      free(key_data);

      if (rc != 0) {
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_TRANSPORT_ERROR;
      }
    }

    /* Load CA certificate for server verification */
    if (config->ca_file) {
      FILE *ca_fp = fopen(config->ca_file, "rb");
      if (!ca_fp) {
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_IO_ERROR;
      }
      fseek(ca_fp, 0, SEEK_END);
      long ca_size = ftell(ca_fp);
      fseek(ca_fp, 0, SEEK_SET);
      char *ca_data = (char *)malloc(ca_size + 1);
      if (!ca_data) {
        fclose(ca_fp);
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_ALLOC_FAILED;
      }
      fread(ca_data, 1, ca_size, ca_fp);
      ca_data[ca_size] = '\0';
      fclose(ca_fp);

      rc = turbo_tls_context_add_trusted_certs(&client->proto.tls.context, ca_data, ca_size);
      free(ca_data);

      if (rc != 0) {
        uv_mutex_unlock(&client->mutex);
        return SYNC_CLIENT_STATUS_TRANSPORT_ERROR;
      }
    }

    /* Set verification flags */
    int verify_flags = config->verify_peer ? TURBO_TLS_VERIFY_PEER_CERT : TURBO_TLS_VERIFY_NONE;
    turbo_tls_context_set_verify_flags(&client->proto.tls.context, verify_flags);
  }

  uv_mutex_unlock(&client->mutex);
  return SYNC_CLIENT_STATUS_OK;
}

sync_client_status_t sync_client_set_ws_config(sync_client_t *client,
                                               const sync_client_ws_config_t *config) {
  if (!client || !config)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  if (client->transport != SYNC_CLIENT_TRANSPORT_WEBSOCKET)
    return SYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Copy inputs to owned buffers to avoid dangling stack pointers */
  char *path_copy = client_common_strdup(config->path ? config->path : "/");
  if (!path_copy)
    return SYNC_CLIENT_STATUS_ALLOC_FAILED;

  char *origin_copy = NULL;
  if (config->origin) {
    origin_copy = client_common_strdup(config->origin);
    if (!origin_copy) {
      free(path_copy);
      return SYNC_CLIENT_STATUS_ALLOC_FAILED;
    }
  }

  char **subp_copy = NULL;
  int subp_count = 0;
  if (config->subprotocols && config->subprotocol_count > 0) {
    subp_count = config->subprotocol_count;
    subp_copy = (char **)calloc((size_t)subp_count, sizeof(char *));
    if (!subp_copy) {
      free(path_copy);
      free(origin_copy);
      return SYNC_CLIENT_STATUS_ALLOC_FAILED;
    }
    for (int i = 0; i < subp_count; i++) {
      if (config->subprotocols[i]) {
        subp_copy[i] = client_common_strdup(config->subprotocols[i]);
        if (!subp_copy[i]) {
          for (int j = 0; j < i; j++) {
            free(subp_copy[j]);
          }
          free(subp_copy);
          free(path_copy);
          free(origin_copy);
          return SYNC_CLIENT_STATUS_ALLOC_FAILED;
        }
      }
    }
  }

  uv_mutex_lock(&client->mutex);

  /* Replace existing config with owned copies */
  ws_free_config_strings(client);

  client->proto.ws.path_owned = path_copy;
  client->proto.ws.origin_owned = origin_copy;
  client->proto.ws.subprotocols_owned = subp_copy;
  client->proto.ws.subprotocols_owned_count = subp_count;

  client->proto.ws.config.path = path_copy;
  client->proto.ws.config.origin = origin_copy;
  client->proto.ws.config.subprotocols = (const char *const *)subp_copy;
  client->proto.ws.config.subprotocol_count = subp_count;
  client->proto.ws.config.use_tls = config->use_tls;

  uv_mutex_unlock(&client->mutex);
  return SYNC_CLIENT_STATUS_OK;
}
