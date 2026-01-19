#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "stb_sprintf.h"
#include <string.h>
#include <time.h>
#include "tlog.h"

#include <uv.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include "turbo_async_server.h"
#include "client_common.h"
#include "config.h"
#include "turbo_dns.h"
#include "arena_buffer.h"
#include "turbo_kcp.h"
#include "turbo_pipe.h"
#include "turbo_tcp.h"
#include "turbo_tls.h"
#include "turbo_websocket_server.h"

#define ASYNC_SERVER_ERROR_MESSAGE_MAX 128
#define ASYNC_SERVER_DEFAULT_BACKLOG 128
#define UNUSED(x) (void)(x)

#ifndef CONTAINER_OF
  #define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

typedef enum {
  COMMAND_LISTEN,
  COMMAND_SEND,
  COMMAND_SENDV,
  COMMAND_SENDV_SLICES,  /* NEW: True zero-copy with arena slices */
  COMMAND_BROADCAST,
  COMMAND_CLOSE_CONNECTION,
  COMMAND_STOP
} async_server_command_type_t;

typedef struct async_server_command_s {
  async_server_command_type_t type;
  union {
    struct {
      char *host;
      int port;
      int backlog;
    } listen;
    struct {
      async_server_connection_t *connection;
      char *data;
      size_t len;
    } send;
    struct {
      async_server_connection_t *connection;
      async_server_iovec_t *iov;
      size_t iovcnt;
    } sendv;
    struct {
      async_server_connection_t *connection;
      turbo_arena_slice_t *slices;  /* Arena slices with refcount */
      size_t slice_count;
    } sendv_slices;
    struct {
      char *data;
      size_t len;
    } broadcast;
    struct {
      async_server_connection_t *connection;
    } close_connection;
  } payload;
  struct async_server_command_s *next;
} async_server_command_t;

/* Forward declarations */
static async_server_status_t transport_setup(async_server_t *server);
static void transport_close(async_server_t *server);

static void handle_listen_command(async_server_t *server, async_server_command_t *cmd);
static void handle_send_command(async_server_t *server, async_server_command_t *cmd);
static void handle_sendv_command(async_server_t *server, async_server_command_t *cmd);
static void handle_sendv_slices_command(async_server_t *server, async_server_command_t *cmd);
static void handle_broadcast_command(async_server_t *server, async_server_command_t *cmd);
static void handle_close_connection_command(async_server_t *server, async_server_command_t *cmd);
static void handle_stop_command(async_server_t *server);

/* Phase 5a: Helper functions for handle_sendv_command refactoring */
static size_t calculate_iov_total_bytes(const async_server_iovec_t *iov, size_t iovcnt);
static void update_sendv_stats(async_server_t *server, async_server_connection_t *connection,
                                size_t total_bytes, size_t iovcnt);
static int tcp_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                          size_t iovcnt);
static int udp_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                          const async_server_iovec_t *iov, size_t iovcnt);
static int kcp_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                          size_t iovcnt);
static int tls_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                          size_t iovcnt);
static int pipe_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                           size_t iovcnt);

/* Phase 5c: Helper functions for handle_listen_command refactoring */
static int tcp_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int udp_listen_impl(async_server_t *server, const char *host, int port);
static int kcp_listen_impl(async_server_t *server, const char *host, int port);
static int tls_listen_impl(async_server_t *server, const char *host, int port);
static int pipe_listen_impl(async_server_t *server, const char *host);

static void tcp_connection_cb(uv_stream_t *server_handle, int status);
static void tcp_write_cb(uv_write_t *req, int status);
static void tcp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void tcp_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void tcp_close_cb(uv_handle_t *handle);

static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void udp_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags);
static void udp_send_cb(uv_udp_send_t *req, int status);

static int kcp_recv_wrapper(void *handle, const turbo_arena_slice_t *data, void *peer);
static void kcp_accept_wrapper(void *server_handle, void *client_handle, void *peer);

static int tls_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);
static void tls_accept_cb(turbo_tls_server_t *tls_server, turbo_tls_client_t *client, void *peer);
static void tls_close_cb(void *handle);

static int pipe_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);
static void pipe_accept_cb(void *handle, int status, void *peer);
static void pipe_close_cb(void *handle);

static void emit_event(async_server_t *server, async_server_event_type_t type,
                       async_server_connection_t *connection, const char *data, size_t length,
                       int status, const char *message, const turbo_arena_slice_t *slice,
                       async_server_event_flags_t flags);
static void emit_listening(async_server_t *server);
static void emit_connection(async_server_t *server, async_server_connection_t *connection);
static void emit_disconnection(async_server_t *server, async_server_connection_t *connection);
static void emit_closed(async_server_t *server);
static void emit_data(async_server_t *server, async_server_connection_t *connection, char *data,
                      size_t length, int free_after);
static void emit_data_slice(async_server_t *server, async_server_connection_t *connection,
                            const turbo_arena_slice_t *slice, int release_after);
static void emit_error_message(async_server_t *server, int status, const char *context);
static void emit_uv_error(async_server_t *server, int status, const char *context);

/* Static context strings for emit_uv_error to avoid ASan false positives with string literal redzones */
static const char CTX_TCP_CONNECTION[] = "tcp connection\0\0\0\0";
static const char CTX_TCP_WRITE[] = "tcp write\0\0\0\0";
static const char CTX_TCP_READ[] = "tcp read\0\0\0\0";
static const char CTX_UDP_RECV[] = "udp recv\0\0\0\0";

static async_server_connection_t *connection_create(async_server_t *server);
static void connection_destroy(async_server_connection_t *connection);
static void connection_add(async_server_t *server, async_server_connection_t *connection);
static void connection_remove(async_server_t *server, async_server_connection_t *connection);

typedef struct {
  uv_tcp_t handle;
  int active;
} async_server_tcp_state_t;

typedef struct async_server_tcp_send_req_s {
  uv_write_t req;
  char *buffer;
  size_t length;
  async_server_connection_t *connection;
  int is_sendv;
} async_server_tcp_send_req_t;

typedef struct async_server_tcp_sendv_req_s {
  uv_write_t req;
  uv_buf_t *bufs;
  size_t bufcnt;
  char **buffers;
  async_server_connection_t *connection;
  int is_sendv;
} async_server_tcp_sendv_req_t;

typedef struct {
  uv_udp_t handle;
  int active;
} async_server_udp_state_t;

typedef struct async_server_udp_send_req_s {
  uv_udp_send_t req;
  char *buffer;
  size_t length;
  struct sockaddr_storage addr;
  int is_sendv;
} async_server_udp_send_req_t;

typedef struct async_server_udp_sendv_req_s {
  uv_udp_send_t req;
  uv_buf_t *bufs;
  size_t bufcnt;
  char **buffers;
  struct sockaddr_storage addr;
  int is_sendv;
} async_server_udp_sendv_req_t;

typedef struct {
  turbo_kcp_server_t server;
  int initialized;
} async_server_kcp_state_t;

typedef struct {
  turbo_tls_context_t context;
  turbo_tls_server_t *server;
  int context_initialized;
  int server_created;
  async_server_tls_config_t config;
} async_server_tls_state_t;

typedef struct {
  turbo_pipe_server_t *server;
} async_server_pipe_state_t;

typedef struct {
  turbo_websocket_server_t *server;
  turbo_websocket_server_config_t config;
  int use_tls;
  int config_set;
} async_server_ws_state_t;

/* Connection structure - internal implementation (opaque to users) */
struct async_server_connection_s {
  async_server_t *server;
  void *user_data;

  union {
    uv_tcp_t tcp;
    struct {
      struct sockaddr_storage addr;
      int addr_len;
    } udp;
    void *kcp_peer;
    turbo_tls_client_t *tls;
    turbo_pipe_client_t *pipe;
    turbo_websocket_connection_t *ws_conn;
  } handle;

  char remote_address[64];
  int remote_port;
  char local_address[64];
  int local_port;

  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t connect_time;
  uint64_t last_activity_time;

  int active;
  int closing;
  int close_callback_registered;  /* NEW: Explicit ownership - did we register close callback? */

  async_server_connection_t *next;
  async_server_connection_t *prev;
};

/* ========================================================================
 * Transport operations vtable - eliminates all protocol switch statements
 *
 * Each protocol (TCP/UDP/KCP/TLS/PIPE) implements this interface.
 * Main code calls server->ops->xxx() without knowing which protocol.
 *
 * This is "good taste" - no special cases, just polymorphism.
 * ======================================================================== */

typedef struct async_server_s async_server_t;

typedef struct async_server_transport_ops_s {
  /* Initialize transport-specific state (called during loop setup) */
  async_server_status_t (*setup)(async_server_t *server);

  /* Close transport-specific handles */
  void (*close)(async_server_t *server);

  /* Start listening on host:port */
  int (*listen)(async_server_t *server, const char *host, int port, int backlog);

  /* Send data to a connection */
  int (*send)(async_server_t *server, async_server_connection_t *connection, char *data, size_t len);

  /* Send scatter-gather IOV array to a connection */
  int (*sendv)(async_server_t *server, async_server_connection_t *connection,
               const async_server_iovec_t *iov, size_t iovcnt);

  /* Optional protocol-specific methods (NULL if not supported) */
  async_server_status_t (*join_multicast_group)(async_server_t *server, const char *group_addr, const char *interface_addr);
  async_server_status_t (*leave_multicast_group)(async_server_t *server, const char *group_addr, const char *interface_addr);
  async_server_status_t (*set_multicast_ttl)(async_server_t *server, int ttl);
  async_server_status_t (*set_multicast_loop)(async_server_t *server, int on);

  /* Protocol name for debugging */
  const char *name;
} async_server_transport_ops_t;

struct async_server_s {
  uv_loop_t loop;
  uv_thread_t loop_thread;
  uv_async_t command_async;

  uv_mutex_t mutex;
  uv_cond_t cond;

  async_server_command_t *command_head;
  async_server_command_t *command_tail;

  async_server_transport_t transport;
  const async_server_transport_ops_t *ops; /* Vtable - no more switch! */

  /* Phase 3: Union optimization - only one protocol active at a time */
  union {
    async_server_tcp_state_t tcp;
    async_server_udp_state_t udp;
    async_server_kcp_state_t kcp;
    async_server_tls_state_t tls;
    async_server_pipe_state_t pipe;
    async_server_ws_state_t ws;
  } proto;

  async_server_event_cb callback;
  void *callback_user_data;

  int loop_ready;
  int loop_running;
  int shutting_down;
  int init_error;
  int listening;

  size_t max_connections;
  int idle_timeout_ms;

  async_server_connection_t *connections_head;
  async_server_connection_t *connections_tail;
  size_t connection_count;

  char event_message[ASYNC_SERVER_ERROR_MESSAGE_MAX];

  async_server_stats_t stats;

  int config_acquired;

  /* Zero-copy arena memory management */
  turbo_arena_t arena;

  async_server_t *global_next;
};

CLIENT_COMMON_DEFINE_PIPE_CLIENT_LIST(async_server, async_server_t);
#define g_server_list_once g_async_server_client_list_once
#define g_server_list_lock g_async_server_client_list_lock
#define g_server_list_initialized g_async_server_client_list_initialized
#define g_server_list_head g_async_server_client_list_head

static void server_list_init_once(void) {
  if (uv_mutex_init(&g_server_list_lock) == 0)
    g_server_list_initialized = 1;
}

static void server_list_register(async_server_t *server) {
  uv_once(&g_server_list_once, server_list_init_once);
  if (!g_server_list_initialized)
    return;
  uv_mutex_lock(&g_server_list_lock);
  server->global_next = g_server_list_head;
  g_server_list_head = server;
  uv_mutex_unlock(&g_server_list_lock);
}

static void server_list_unregister(async_server_t *server) {
  if (!g_server_list_initialized)
    return;
  uv_mutex_lock(&g_server_list_lock);
  async_server_t **cursor = &g_server_list_head;
  while (*cursor) {
    if (*cursor == server) {
      *cursor = server->global_next;
      break;
    }
    cursor = &(*cursor)->global_next;
  }
  server->global_next = NULL;
  uv_mutex_unlock(&g_server_list_lock);
}

static async_server_t *server_from_pipe(turbo_pipe_server_t *pipe_server) {
  if (!g_server_list_initialized)
    return NULL;
  uv_mutex_lock(&g_server_list_lock);
  async_server_t *cursor = g_server_list_head;
  while (cursor) {
    if (cursor->transport == ASYNC_SERVER_TRANSPORT_PIPE && cursor->proto.pipe.server == pipe_server) {
      uv_mutex_unlock(&g_server_list_lock);
      return cursor;
    }
    cursor = cursor->global_next;
  }
  uv_mutex_unlock(&g_server_list_lock);
  return NULL;
}

static async_server_t *server_from_tls_impl(turbo_tls_server_t *tls_server) {
  if (!g_server_list_initialized || !tls_server)
    return NULL;
  uv_mutex_lock(&g_server_list_lock);
  async_server_t *cursor = g_server_list_head;
  while (cursor) {
    if (cursor->transport == ASYNC_SERVER_TRANSPORT_TLS && cursor->proto.tls.server == tls_server) {
      uv_mutex_unlock(&g_server_list_lock);
      return cursor;
    }
    cursor = cursor->global_next;
  }
  uv_mutex_unlock(&g_server_list_lock);
  return NULL;
}

static async_server_t *server_from_kcp_impl(turbo_kcp_server_t *kcp_server) {
  if (!kcp_server)
    return NULL;
  return CONTAINER_OF(kcp_server, async_server_t, proto.kcp.server);
}

/* KCP callback wrapper implementations */
static int kcp_recv_wrapper(void *handle, const turbo_arena_slice_t *data, void *peer) {
  turbo_kcp_server_t *kcp_server = (turbo_kcp_server_t *)handle;
  async_server_t *server = server_from_kcp_impl(kcp_server);
  if (!server || !data || data->length == 0)
    return 0;

  /* Find connection for this peer */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->handle.kcp_peer == peer)
      break;
    conn = conn->next;
  }

  if (!conn) {
    return 0;
  }

  uv_mutex_lock(&server->mutex);
  server->stats.messages_received++;
  server->stats.bytes_received += data->length;
  conn->bytes_received += data->length;
  uv_mutex_unlock(&server->mutex);

  emit_data_slice(server, conn, data, 0);
  return 0;
}

static void kcp_accept_wrapper(void *server_handle, void *client_handle, void *peer) {
  turbo_kcp_server_t *kcp_server = (turbo_kcp_server_t *)server_handle;
  async_server_t *server = server_from_kcp_impl(kcp_server);
  if (!server)
    return;

  if (server->max_connections > 0 && server->connection_count >= server->max_connections) {
    uv_mutex_lock(&server->mutex);
    server->stats.rejected_connections++;
    uv_mutex_unlock(&server->mutex);
    return;
  }

  async_server_connection_t *conn = connection_create(server);
  if (!conn)
    return;

  conn->handle.kcp_peer = client_handle; /* client_handle is the peer */
  connection_add(server, conn);

  uv_mutex_lock(&server->mutex);
  server->stats.total_connections++;
  uv_mutex_unlock(&server->mutex);

  emit_connection(server, conn);
}

static async_server_command_t *command_create(async_server_command_type_t type) {
  async_server_command_t *cmd = (async_server_command_t *)calloc(1, sizeof(*cmd));
  if (!cmd)
    return NULL;
  cmd->type = type;
  return cmd;
}

static async_server_command_t *command_create_listen(const char *host, int port, int backlog) {
  async_server_command_t *cmd = command_create(COMMAND_LISTEN);
  if (!cmd)
    return NULL;
  if (host) {
    cmd->payload.listen.host = client_common_strdup(host);
    if (!cmd->payload.listen.host) {
      free(cmd);
      return NULL;
    }
  }
  cmd->payload.listen.port = port;
  cmd->payload.listen.backlog = backlog > 0 ? backlog : ASYNC_SERVER_DEFAULT_BACKLOG;
  return cmd;
}

static async_server_command_t *command_create_send(async_server_connection_t *connection,
                                                   const char *data, size_t len) {
  async_server_command_t *cmd = command_create(COMMAND_SEND);
  if (!cmd)
    return NULL;
  cmd->payload.send.connection = connection;
  if (len > 0 && data) {
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

static async_server_command_t *command_create_sendv(async_server_connection_t *connection,
                                                    const async_server_iovec_t *iov,
                                                    size_t iovcnt) {
  if (!iov || iovcnt == 0)
    return NULL;

  size_t cmd_size = sizeof(async_server_command_t);
  size_t iov_size = iovcnt * sizeof(async_server_iovec_t);
  size_t total_size = cmd_size + iov_size;

  void *mem = malloc(total_size);
  if (!mem)
    return NULL;

  async_server_command_t *cmd = (async_server_command_t *)mem;
  async_server_iovec_t *iov_array = (async_server_iovec_t *)((char *)mem + cmd_size);

  memset(cmd, 0, sizeof(async_server_command_t));
  cmd->type = COMMAND_SENDV;
  cmd->payload.sendv.connection = connection;
  cmd->payload.sendv.iov = iov_array;
  cmd->payload.sendv.iovcnt = iovcnt;

  for (size_t i = 0; i < iovcnt; i++) {
    iov_array[i].data = iov[i].data;
    iov_array[i].len = iov[i].len;
  }

  return cmd;
}

static async_server_command_t *command_create_sendv_slices(async_server_connection_t *connection,
                                                            const turbo_arena_slice_t *slices,
                                                            size_t slice_count) {
  if (!slices || slice_count == 0)
    return NULL;

  async_server_command_t *cmd = command_create(COMMAND_SENDV_SLICES);
  if (!cmd)
    return NULL;

  /* Allocate slice array (command owns this array) */
  cmd->payload.sendv_slices.slices = (turbo_arena_slice_t *)malloc(slice_count * sizeof(turbo_arena_slice_t));
  if (!cmd->payload.sendv_slices.slices) {
    free(cmd);
    return NULL;
  }

  /* Copy slices and increase refcount */
  for (size_t i = 0; i < slice_count; i++) {
    cmd->payload.sendv_slices.slices[i] = slices[i];

    /* Increase refcount on buffer - ensures it stays valid until we're done */
    if (slices[i].buffer) {
      turbo_arena_buffer_ref(slices[i].buffer);
    }
  }

  cmd->payload.sendv_slices.connection = connection;
  cmd->payload.sendv_slices.slice_count = slice_count;
  return cmd;
}

static async_server_command_t *command_create_broadcast(const char *data, size_t len) {
  async_server_command_t *cmd = command_create(COMMAND_BROADCAST);
  if (!cmd)
    return NULL;
  if (len > 0 && data) {
    cmd->payload.broadcast.data = (char *)malloc(len);
    if (!cmd->payload.broadcast.data) {
      free(cmd);
      return NULL;
    }
    memcpy(cmd->payload.broadcast.data, data, len);
    cmd->payload.broadcast.len = len;
  }
  return cmd;
}

static async_server_command_t *
command_create_close_connection(async_server_connection_t *connection) {
  async_server_command_t *cmd = command_create(COMMAND_CLOSE_CONNECTION);
  if (!cmd)
    return NULL;
  cmd->payload.close_connection.connection = connection;
  return cmd;
}

static async_server_command_t *command_create_stop(void) { return command_create(COMMAND_STOP); }

static void free_command(async_server_command_t *cmd) {
  if (!cmd)
    return;
  if (cmd->type == COMMAND_LISTEN) {
    free(cmd->payload.listen.host);
  } else if (cmd->type == COMMAND_SEND) {
    free(cmd->payload.send.data);
  } else if (cmd->type == COMMAND_SENDV) {
    /* Zero-copy: only command + iov array */
  } else if (cmd->type == COMMAND_SENDV_SLICES) {
    /* Release all arena slice references */
    if (cmd->payload.sendv_slices.slices) {
      for (size_t i = 0; i < cmd->payload.sendv_slices.slice_count; i++) {
        if (cmd->payload.sendv_slices.slices[i].buffer) {
          turbo_arena_buffer_unref(cmd->payload.sendv_slices.slices[i].buffer);
        }
      }
      free(cmd->payload.sendv_slices.slices);
    }
  } else if (cmd->type == COMMAND_BROADCAST) {
    free(cmd->payload.broadcast.data);
  }
  free(cmd);
}

static async_server_status_t submit_command(async_server_t *server, async_server_command_t *cmd,
                                            int mark_shutdown) {
  if (!server || !cmd)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  async_server_status_t status = ASYNC_SERVER_STATUS_OK;

  uv_mutex_lock(&server->mutex);
  if (!server->loop_running) {
    status = ASYNC_SERVER_STATUS_NOT_READY;
  } else if (server->shutting_down) {
    status = ASYNC_SERVER_STATUS_SHUTTING_DOWN;
  } else {
    if (mark_shutdown)
      server->shutting_down = 1;
    cmd->next = NULL;
    if (server->command_tail)
      server->command_tail->next = cmd;
    else
      server->command_head = cmd;
    server->command_tail = cmd;
  }
  uv_mutex_unlock(&server->mutex);

  if (status == ASYNC_SERVER_STATUS_OK)
    uv_async_send(&server->command_async);

  return status;
}

static void command_async_cb(uv_async_t *handle) {
  async_server_t *server = (async_server_t *)handle->data;

  for (;;) {
    async_server_command_t *cmd = NULL;

    uv_mutex_lock(&server->mutex);
    if (server->command_head) {
      cmd = server->command_head;
      server->command_head = cmd->next;
      if (!server->command_head)
        server->command_tail = NULL;
    }
    int shutting_down = server->shutting_down;
    uv_mutex_unlock(&server->mutex);

    if (!cmd) {
      if (shutting_down)
        transport_close(server);
      break;
    }

    switch (cmd->type) {
    case COMMAND_LISTEN:
      handle_listen_command(server, cmd);
      break;
    case COMMAND_SEND:
      handle_send_command(server, cmd);
      break;
    case COMMAND_SENDV:
      handle_sendv_command(server, cmd);
      break;
    case COMMAND_SENDV_SLICES:
      handle_sendv_slices_command(server, cmd);
      break;
    case COMMAND_BROADCAST:
      handle_broadcast_command(server, cmd);
      break;
    case COMMAND_CLOSE_CONNECTION:
      handle_close_connection_command(server, cmd);
      break;
    case COMMAND_STOP:
      handle_stop_command(server);
      break;
    }

    free_command(cmd);
  }
}

static void loop_thread_main(void *arg) {
  async_server_t *server = (async_server_t *)arg;
  int rc = uv_loop_init(&server->loop);
  int async_initialized = 0;

  if (rc == 0) {
    rc = uv_async_init(&server->loop, &server->command_async, command_async_cb);
    if (rc == 0) {
      async_initialized = 1;
      server->command_async.data = server;
    }
  }

  if (rc == 0) {
    async_server_status_t setup_status = transport_setup(server);
    if (setup_status != ASYNC_SERVER_STATUS_OK)
      rc = UV_EINVAL;
  }

  uv_mutex_lock(&server->mutex);
  server->init_error = rc;
  server->loop_ready = 1;
  server->loop_running = (rc == 0);
  uv_cond_broadcast(&server->cond);
  uv_mutex_unlock(&server->mutex);

  if (rc != 0) {
    if (async_initialized && !uv_is_closing((uv_handle_t *)&server->command_async)) {
      uv_close((uv_handle_t *)&server->command_async, NULL);
      uv_run(&server->loop, UV_RUN_DEFAULT);
    }
    uv_loop_close(&server->loop);
    return;
  }

  uv_run(&server->loop, UV_RUN_DEFAULT);

  uv_mutex_lock(&server->mutex);
  server->loop_running = 0;
  uv_cond_broadcast(&server->cond);
  uv_mutex_unlock(&server->mutex);

  transport_close(server);

  if (!uv_is_closing((uv_handle_t *)&server->command_async)) {
    uv_close((uv_handle_t *)&server->command_async, NULL);
  }

  /* Run loop to completion - process all close callbacks.
   * This ensures all connections are properly closed before thread exits.
   * Keep running until loop has no more active handles. */
  while (uv_loop_alive(&server->loop)) {
    uv_run(&server->loop, UV_RUN_ONCE);
  }

  uv_loop_close(&server->loop);
}

/* ========================================================================
 * Vtable forward declarations
 * ======================================================================== */
static async_server_status_t server_tcp_setup_impl(async_server_t *server);
static void server_tcp_close_impl(async_server_t *server);
static int server_tcp_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int server_tcp_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len);
static int server_tcp_sendv_impl(async_server_t *server, async_server_connection_t *conn, const async_server_iovec_t *iov, size_t iovcnt);

static async_server_status_t server_udp_setup_impl(async_server_t *server);
static void server_udp_close_impl(async_server_t *server);
static int server_udp_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int server_udp_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len);
static int server_udp_sendv_impl(async_server_t *server, async_server_connection_t *conn, const async_server_iovec_t *iov, size_t iovcnt);
static async_server_status_t server_udp_join_multicast_group_impl(async_server_t *server, const char *group_addr, const char *interface_addr);
static async_server_status_t server_udp_leave_multicast_group_impl(async_server_t *server, const char *group_addr, const char *interface_addr);
static async_server_status_t server_udp_set_multicast_ttl_impl(async_server_t *server, int ttl);
static async_server_status_t server_udp_set_multicast_loop_impl(async_server_t *server, int on);

static async_server_status_t server_kcp_setup_impl(async_server_t *server);
static void server_kcp_close_impl(async_server_t *server);
static int server_kcp_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int server_kcp_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len);
static int server_kcp_sendv_impl(async_server_t *server, async_server_connection_t *conn, const async_server_iovec_t *iov, size_t iovcnt);

static async_server_status_t server_tls_setup_impl(async_server_t *server);
static void server_tls_close_impl(async_server_t *server);
static int server_tls_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int server_tls_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len);
static int server_tls_sendv_impl(async_server_t *server, async_server_connection_t *conn, const async_server_iovec_t *iov, size_t iovcnt);

static async_server_status_t server_pipe_setup_impl(async_server_t *server);
static void server_pipe_close_impl(async_server_t *server);
static int server_pipe_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int server_pipe_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len);
static int server_pipe_sendv_impl(async_server_t *server, async_server_connection_t *conn, const async_server_iovec_t *iov, size_t iovcnt);

static async_server_status_t server_ws_setup_impl(async_server_t *server);
static void server_ws_close_impl(async_server_t *server);
static int server_ws_listen_impl(async_server_t *server, const char *host, int port, int backlog);
static int server_ws_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len);
static int server_ws_sendv_impl(async_server_t *server, async_server_connection_t *conn, const async_server_iovec_t *iov, size_t iovcnt);

/* Static vtable instances */
static const async_server_transport_ops_t server_tcp_ops = {
    .setup = server_tcp_setup_impl,
    .close = server_tcp_close_impl,
    .listen = server_tcp_listen_impl,
    .send = server_tcp_send_impl,
    .sendv = server_tcp_sendv_impl,
    .join_multicast_group = NULL,  /* Not supported */
    .leave_multicast_group = NULL, /* Not supported */
    .set_multicast_ttl = NULL,     /* Not supported */
    .set_multicast_loop = NULL,    /* Not supported */
    .name = "TCP\0\0\0\0"};

static const async_server_transport_ops_t server_udp_ops = {
    .setup = server_udp_setup_impl,
    .close = server_udp_close_impl,
    .listen = server_udp_listen_impl,
    .send = server_udp_send_impl,
    .sendv = server_udp_sendv_impl,
    .join_multicast_group = server_udp_join_multicast_group_impl,   /* UDP-specific */
    .leave_multicast_group = server_udp_leave_multicast_group_impl, /* UDP-specific */
    .set_multicast_ttl = server_udp_set_multicast_ttl_impl,         /* UDP-specific */
    .set_multicast_loop = server_udp_set_multicast_loop_impl,       /* UDP-specific */
    .name = "UDP\0\0\0\0"};

static const async_server_transport_ops_t server_kcp_ops = {
    .setup = server_kcp_setup_impl,
    .close = server_kcp_close_impl,
    .listen = server_kcp_listen_impl,
    .send = server_kcp_send_impl,
    .sendv = server_kcp_sendv_impl,
    .join_multicast_group = NULL,  /* Not supported */
    .leave_multicast_group = NULL, /* Not supported */
    .set_multicast_ttl = NULL,     /* Not supported */
    .set_multicast_loop = NULL,    /* Not supported */
    .name = "KCP\0\0\0\0"};

static const async_server_transport_ops_t server_tls_ops = {
    .setup = server_tls_setup_impl,
    .close = server_tls_close_impl,
    .listen = server_tls_listen_impl,
    .send = server_tls_send_impl,
    .sendv = server_tls_sendv_impl,
    .join_multicast_group = NULL,  /* Not supported */
    .leave_multicast_group = NULL, /* Not supported */
    .set_multicast_ttl = NULL,     /* Not supported */
    .set_multicast_loop = NULL,    /* Not supported */
    .name = "TLS\0\0\0\0"};

static const async_server_transport_ops_t server_pipe_ops = {
    .setup = server_pipe_setup_impl,
    .close = server_pipe_close_impl,
    .listen = server_pipe_listen_impl,
    .send = server_pipe_send_impl,
    .sendv = server_pipe_sendv_impl,
    .join_multicast_group = NULL,  /* Not supported */
    .leave_multicast_group = NULL, /* Not supported */
    .set_multicast_ttl = NULL,     /* Not supported */
    .set_multicast_loop = NULL,    /* Not supported */
    .name = "PIPE\0\0\0\0"};

static const async_server_transport_ops_t server_ws_ops = {
    .setup = server_ws_setup_impl,
    .close = server_ws_close_impl,
    .listen = server_ws_listen_impl,
    .send = server_ws_send_impl,
    .sendv = server_ws_sendv_impl,
    .join_multicast_group = NULL,
    .leave_multicast_group = NULL,
    .set_multicast_ttl = NULL,
    .set_multicast_loop = NULL,
    .name = "WEBSOCKET\0\0\0\0"};

/* ========================================================================
 * TCP vtable implementations
 * ======================================================================== */
static async_server_status_t server_tcp_setup_impl(async_server_t *server) {
  int rc = uv_tcp_init(&server->loop, &server->proto.tcp.handle);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_INTERNAL_ERROR;
  server->proto.tcp.handle.data = server;
  server->proto.tcp.active = 0;
  return ASYNC_SERVER_STATUS_OK;
}

static void server_tcp_close_impl(async_server_t *server) {
  if (server->proto.tcp.active)
    uv_read_stop((uv_stream_t *)&server->proto.tcp.handle);
  server->proto.tcp.active = 0;
  if (!uv_is_closing((uv_handle_t *)&server->proto.tcp.handle))
    uv_close((uv_handle_t *)&server->proto.tcp.handle, tcp_close_cb);
  else
    emit_closed(server);
}

static int server_tcp_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  return tcp_listen_impl(server, host, port, backlog);
}

static int server_tcp_send_impl(async_server_t *server, async_server_connection_t *connection,
                                char *data, size_t len) {
  async_server_tcp_send_req_t *send_req =
      (async_server_tcp_send_req_t *)calloc(1, sizeof(*send_req));
  if (!send_req) {
    free(data);
    return -ASYNC_SERVER_STATUS_ALLOC_FAILED;
  }
  send_req->buffer = data;
  send_req->length = len;
  send_req->connection = connection;
  send_req->is_sendv = 0;
  send_req->req.data = send_req;

  uv_buf_t buf = uv_buf_init(send_req->buffer, (unsigned int)send_req->length);
  int rc = uv_write(&send_req->req, (uv_stream_t *)&connection->handle.tcp, &buf, 1, tcp_write_cb);
  if (rc != 0) {
    free(send_req->buffer);
    free(send_req);
    return rc;
  }
  return 0;
}

static int server_tcp_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                                 const async_server_iovec_t *iov, size_t iovcnt) {
  (void)server;
  return tcp_sendv_impl(connection, iov, iovcnt);
}

/* ========================================================================
 * UDP vtable implementations
 * ======================================================================== */
static async_server_status_t server_udp_setup_impl(async_server_t *server) {
  int rc = uv_udp_init(&server->loop, &server->proto.udp.handle);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_INTERNAL_ERROR;
  server->proto.udp.handle.data = server;
  server->proto.udp.active = 0;
  return ASYNC_SERVER_STATUS_OK;
}

static void server_udp_close_impl(async_server_t *server) {
  if (server->proto.udp.active)
    uv_udp_recv_stop(&server->proto.udp.handle);
  server->proto.udp.active = 0;
  if (!uv_is_closing((uv_handle_t *)&server->proto.udp.handle))
    uv_close((uv_handle_t *)&server->proto.udp.handle, NULL);
  emit_closed(server);
}

static int server_udp_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  (void)backlog;
  return udp_listen_impl(server, host, port);
}

static int server_udp_send_impl(async_server_t *server, async_server_connection_t *connection,
                                char *data, size_t len) {
  async_server_udp_send_req_t *send_req =
      (async_server_udp_send_req_t *)calloc(1, sizeof(*send_req));
  if (!send_req) {
    free(data);
    return -ASYNC_SERVER_STATUS_ALLOC_FAILED;
  }
  send_req->buffer = data;
  send_req->length = len;
  memcpy(&send_req->addr, &connection->handle.udp.addr, sizeof(send_req->addr));
  send_req->is_sendv = 0;
  send_req->req.data = send_req;
  uv_buf_t buf = uv_buf_init(send_req->buffer, (unsigned int)send_req->length);
  int rc = uv_udp_send(&send_req->req, &server->proto.udp.handle, &buf, 1,
                       (const struct sockaddr *)&send_req->addr, udp_send_cb);
  if (rc != 0) {
    free(send_req->buffer);
    free(send_req);
    return rc;
  }
  return 0;
}

static int server_udp_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                                 const async_server_iovec_t *iov, size_t iovcnt) {
  return udp_sendv_impl(server, connection, iov, iovcnt);
}

/* UDP-specific: multicast group join */
static async_server_status_t server_udp_join_multicast_group_impl(async_server_t *server,
                                                                   const char *group_addr,
                                                                   const char *interface_addr) {
  if (!group_addr)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  int rc = uv_udp_set_membership(&server->proto.udp.handle, group_addr, interface_addr,
                                  UV_JOIN_GROUP);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_IO_ERROR;

  return ASYNC_SERVER_STATUS_OK;
}

/* UDP-specific: multicast group leave */
static async_server_status_t server_udp_leave_multicast_group_impl(async_server_t *server,
                                                                    const char *group_addr,
                                                                    const char *interface_addr) {
  if (!group_addr)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  int rc = uv_udp_set_membership(&server->proto.udp.handle, group_addr, interface_addr,
                                  UV_LEAVE_GROUP);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_IO_ERROR;

  return ASYNC_SERVER_STATUS_OK;
}

/* UDP-specific: multicast TTL */
static async_server_status_t server_udp_set_multicast_ttl_impl(async_server_t *server, int ttl) {
  if (ttl < 1 || ttl > 255)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  int rc = uv_udp_set_multicast_ttl(&server->proto.udp.handle, ttl);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_IO_ERROR;

  return ASYNC_SERVER_STATUS_OK;
}

/* UDP-specific: multicast loopback */
static async_server_status_t server_udp_set_multicast_loop_impl(async_server_t *server, int on) {
  int rc = uv_udp_set_multicast_loop(&server->proto.udp.handle, on);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_IO_ERROR;

  return ASYNC_SERVER_STATUS_OK;
}

/* ========================================================================
 * KCP vtable implementations
 * ======================================================================== */
static async_server_status_t server_kcp_setup_impl(async_server_t *server) {
  server->proto.kcp.initialized = 0;
  return ASYNC_SERVER_STATUS_OK;
}

static void server_kcp_close_impl(async_server_t *server) {
  if (server->proto.kcp.initialized) {
    turbo_kcp_server_stop(&server->proto.kcp.server);
    server->proto.kcp.initialized = 0;
  }
  emit_closed(server);
}

static int server_kcp_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  (void)backlog;
  return kcp_listen_impl(server, host, port);
}

static int server_kcp_send_impl(async_server_t *server, async_server_connection_t *connection,
                                char *data, size_t len) {
  (void)server;
  int rc = turbo_kcp_send((turbo_kcp_client_t *)connection->handle.kcp_peer, data, len);
  free(data);
  return rc;
}

static int server_kcp_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                                 const async_server_iovec_t *iov, size_t iovcnt) {
  (void)server;
  return kcp_sendv_impl(connection, iov, iovcnt);
}

/* ========================================================================
 * TLS vtable implementations
 * ======================================================================== */
static async_server_status_t server_tls_setup_impl(async_server_t *server) {
  server->proto.tls.server = NULL;
  server->proto.tls.context_initialized = 0;
  server->proto.tls.server_created = 0;
  return ASYNC_SERVER_STATUS_OK;
}

static void server_tls_close_impl(async_server_t *server) {
  if (server->proto.tls.server) {
    turbo_tls_server_stop(server->proto.tls.server);
    server->proto.tls.server = NULL;
  }
  if (server->proto.tls.context_initialized) {
    turbo_tls_context_destroy(&server->proto.tls.context);
    server->proto.tls.context_initialized = 0;
  }
  emit_closed(server);
}

static int server_tls_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  (void)backlog;
  return tls_listen_impl(server, host, port);
}

static int server_tls_send_impl(async_server_t *server, async_server_connection_t *connection,
                                char *data, size_t len) {
  (void)server;
  int rc = turbo_tls_send(connection->handle.tls, data, len);
  free(data);
  return rc;
}

static int server_tls_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                                 const async_server_iovec_t *iov, size_t iovcnt) {
  (void)server;
  return tls_sendv_impl(connection, iov, iovcnt);
}

/* ========================================================================
 * PIPE vtable implementations
 * ======================================================================== */
static async_server_status_t server_pipe_setup_impl(async_server_t *server) {
  server->proto.pipe.server = NULL;
  return ASYNC_SERVER_STATUS_OK;
}

static void server_pipe_close_impl(async_server_t *server) {
  if (server->proto.pipe.server) {
    turbo_pipe_server_stop(server->proto.pipe.server);
    free(server->proto.pipe.server);
    server->proto.pipe.server = NULL;
  }
  emit_closed(server);
}

static int server_pipe_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  (void)port;
  (void)backlog;
  return pipe_listen_impl(server, host);
}

static int server_pipe_send_impl(async_server_t *server, async_server_connection_t *connection,
                                 char *data, size_t len) {
  (void)server;
  int rc = turbo_pipe_send(connection->handle.pipe, data, len);
  free(data);
  return rc;
}

static int server_pipe_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                                  const async_server_iovec_t *iov, size_t iovcnt) {
  (void)server;
  return pipe_sendv_impl(connection, iov, iovcnt);
}

static async_server_status_t transport_setup(async_server_t *server) {
  /* Dispatch via vtable - NO SWITCH! */
  return server->ops->setup(server);
}

static void transport_close(async_server_t *server) {
  /* Close all connections first (common for all transports) */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    async_server_connection_t *next = conn->next;
    connection_destroy(conn);
    conn = next;
  }
  server->connections_head = NULL;
  server->connections_tail = NULL;
  server->connection_count = 0;

  /* Dispatch via vtable - NO SWITCH! */
  server->ops->close(server);
  server->listening = 0;
}

static async_server_connection_t *connection_create(async_server_t *server) {
  async_server_connection_t *conn =
      (async_server_connection_t *)calloc(1, sizeof(async_server_connection_t));
  if (!conn)
    return NULL;
  conn->server = server;
  conn->connect_time = (uint64_t)time(NULL) * 1000;
  conn->active = 1;
  return conn;
}

static void connection_close_cb(uv_handle_t *handle) {
  /* Get connection from handle */
  async_server_connection_t *connection = (async_server_connection_t *)handle->data;
  if (connection) {
    free(connection);
  }
}

static void connection_destroy(async_server_connection_t *connection) {
  if (!connection)
    return;

  connection_remove(connection->server, connection);

  switch (connection->server->transport) {
  case ASYNC_SERVER_TRANSPORT_TCP:
    /* Explicit ownership tracking - only first call registers callback */
    if (!connection->close_callback_registered) {
      /* First time destroying this connection - we initiate close */
      connection->handle.tcp.data = connection;
      connection->close_callback_registered = 1;
      uv_close((uv_handle_t *)&connection->handle.tcp, connection_close_cb);
      return; /* Callback will free when close completes */
    }

    /* Repeated call - close already in progress */
    /* Callback will free, we do nothing */
    return;

  case ASYNC_SERVER_TRANSPORT_TLS:
    if (connection->handle.tls) {
      turbo_tls_client_close(connection->handle.tls);
      /* TLS has its own close callback (tls_close_cb) - don't free here */
      return;
    }
    break;

  case ASYNC_SERVER_TRANSPORT_PIPE:
    if (connection->handle.pipe) {
      turbo_pipe_client_close(connection->handle.pipe);
      /* PIPE has its own close callback (pipe_close_cb) - don't free here */
      return;
    }
    break;

  case ASYNC_SERVER_TRANSPORT_UDP:
  case ASYNC_SERVER_TRANSPORT_KCP:
    /* UDP/KCP have no close callbacks - safe to free immediately */
    break;

  default:
    break;
  }

  /* Only reach here for UDP/KCP or already-closed handles */
  free(connection);
}

static void connection_add(async_server_t *server, async_server_connection_t *connection) {
  uv_mutex_lock(&server->mutex);
  connection->next = NULL;
  connection->prev = server->connections_tail;
  if (server->connections_tail)
    server->connections_tail->next = connection;
  else
    server->connections_head = connection;
  server->connections_tail = connection;
  server->connection_count++;
  server->stats.active_connections = server->connection_count;
  uv_mutex_unlock(&server->mutex);
}

static void connection_remove(async_server_t *server, async_server_connection_t *connection) {
  uv_mutex_lock(&server->mutex);
  if (connection->prev)
    connection->prev->next = connection->next;
  else
    server->connections_head = connection->next;

  if (connection->next)
    connection->next->prev = connection->prev;
  else
    server->connections_tail = connection->prev;

  if (server->connection_count > 0)
    server->connection_count--;
  server->stats.active_connections = server->connection_count;
  uv_mutex_unlock(&server->mutex);
}

/* Phase 5c: Helper functions for handle_listen_command */
static int tcp_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  struct sockaddr_storage addr;
  int addr_len = 0;
  const char *bind_host = host ? host : "0.0.0.0";
  int rc = turbo_dns_resolve(&server->loop, bind_host, port, &addr, &addr_len);
  if (rc != 0)
    return rc;
  rc = uv_tcp_bind(&server->proto.tcp.handle, (const struct sockaddr *)&addr, 0);
  if (rc != 0)
    return rc;
  rc = uv_listen((uv_stream_t *)&server->proto.tcp.handle, backlog, tcp_connection_cb);
  if (rc != 0)
    return rc;
  server->proto.tcp.active = 1;
  server->listening = 1;
  return 0;
}

static int udp_listen_impl(async_server_t *server, const char *host, int port) {
  struct sockaddr_storage addr;
  int addr_len = 0;
  const char *bind_host = host ? host : "0.0.0.0";
  int rc = turbo_dns_resolve(&server->loop, bind_host, port, &addr, &addr_len);
  if (rc != 0)
    return rc;
  rc = uv_udp_bind(&server->proto.udp.handle, (const struct sockaddr *)&addr, 0);
  if (rc != 0)
    return rc;
  rc = uv_udp_recv_start(&server->proto.udp.handle, udp_alloc_cb, udp_recv_cb);
  if (rc != 0)
    return rc;
  server->proto.udp.active = 1;
  server->listening = 1;
  return 0;
}

static int kcp_listen_impl(async_server_t *server, const char *host, int port) {
  const char *bind_host = host ? host : "0.0.0.0";
  int rc = turbo_kcp_server_init(&server->proto.kcp.server, &server->loop, bind_host, (unsigned short)port);
  if (rc != 0)
    return rc;
  server->proto.kcp.initialized = 1;
  rc = turbo_kcp_server_start(&server->proto.kcp.server, kcp_accept_wrapper, kcp_recv_wrapper);
  if (rc != 0) {
    turbo_kcp_server_stop(&server->proto.kcp.server);
    server->proto.kcp.initialized = 0;
    return rc;
  }
  server->listening = 1;
  return 0;
}

static int tls_listen_impl(async_server_t *server, const char *host, int port) {
  if (!server->proto.tls.context_initialized)
    return -ASYNC_SERVER_STATUS_NOT_READY;
  if (server->proto.tls.server_created && server->proto.tls.server) {
    turbo_tls_server_stop(server->proto.tls.server);
    server->proto.tls.server = NULL;
    server->proto.tls.server_created = 0;
  }
  const char *bind_host = host ? host : "0.0.0.0";
  int rc = turbo_tls_server_init(server->proto.tls.server, &server->loop, &server->proto.tls.context,
                                 bind_host, (unsigned short)port);
  if (rc != 0)
    return rc;
  server->proto.tls.server_created = 1;
  rc = turbo_tls_server_start(server->proto.tls.server, tls_recv_cb, (turbo_connect_cb)tls_accept_cb, tls_close_cb);
  if (rc != 0) {
    turbo_tls_server_stop(server->proto.tls.server);
    server->proto.tls.server = NULL;
    server->proto.tls.server_created = 0;
    return rc;
  }
  server->listening = 1;
  return 0;
}

static int pipe_listen_impl(async_server_t *server, const char *host) {
  if (!server->proto.pipe.server) {
    server->proto.pipe.server = (turbo_pipe_server_t *)calloc(1, sizeof(turbo_pipe_server_t));
    if (!server->proto.pipe.server)
      return -ASYNC_SERVER_STATUS_ALLOC_FAILED;
  }
  int rc = turbo_pipe_server_init(server->proto.pipe.server, &server->loop, host);
  if (rc != 0) {
    free(server->proto.pipe.server);
    server->proto.pipe.server = NULL;
    return rc;
  }
  rc = turbo_pipe_server_start(server->proto.pipe.server, pipe_recv_cb, pipe_accept_cb, pipe_close_cb);
  if (rc != 0) {
    turbo_pipe_server_stop(server->proto.pipe.server);
    free(server->proto.pipe.server);
    server->proto.pipe.server = NULL;
    return rc;
  }
  server->listening = 1;
  return 0;
}

static void handle_listen_command(async_server_t *server, async_server_command_t *cmd) {
  const char *host = cmd->payload.listen.host;
  int port = cmd->payload.listen.port;
  int backlog = cmd->payload.listen.backlog;

  /* Dispatch via vtable - NO SWITCH! */
  int rc = server->ops->listen(server, host, port, backlog);
  if (rc != 0) {
    if (rc == -ASYNC_SERVER_STATUS_NOT_READY)
      emit_error_message(server, rc, "not ready");
    else if (rc == -ASYNC_SERVER_STATUS_ALLOC_FAILED)
      emit_error_message(server, rc, "allocation failed");
    else
      emit_uv_error(server, rc, server->ops->name);
    return;
  }
  TLOG_INFO("Server listening on {:s}:{:d} (backlog: {:d})", host ? host : "0.0.0.0", port, backlog);
  emit_listening(server);
}

static void handle_send_command(async_server_t *server, async_server_command_t *cmd) {
  async_server_connection_t *connection = cmd->payload.send.connection;
  char *data = cmd->payload.send.data;
  size_t len = cmd->payload.send.len;

  if (!connection || !connection->active || !data || len == 0) {
    free(data);
    return;
  }

  cmd->payload.send.data = NULL;

  uv_mutex_lock(&server->mutex);
  server->stats.messages_sent++;
  server->stats.bytes_sent += len;
  connection->bytes_sent += len;
  uv_mutex_unlock(&server->mutex);

  /* Dispatch via vtable - NO SWITCH! */
  int rc = server->ops->send(server, connection, data, len);
  if (rc != 0)
    emit_uv_error(server, rc, server->ops->name);
}

/* Phase 5a: Helper functions for handle_sendv_command */
static size_t calculate_iov_total_bytes(const async_server_iovec_t *iov, size_t iovcnt) {
  size_t total = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    total += iov[i].len;
  }
  return total;
}

static void update_sendv_stats(async_server_t *server, async_server_connection_t *connection,
                                size_t total_bytes, size_t iovcnt) {
  uv_mutex_lock(&server->mutex);
  server->stats.scatter_gather_sends++;
  server->stats.total_iov_buffers_sent += iovcnt;
  server->stats.messages_sent++;
  server->stats.bytes_sent += total_bytes;
  connection->bytes_sent += total_bytes;
  uv_mutex_unlock(&server->mutex);
}

static int tcp_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                          size_t iovcnt) {
  async_server_tcp_sendv_req_t *send_req =
      (async_server_tcp_sendv_req_t *)calloc(1, sizeof(*send_req));
  if (!send_req)
    return UV_ENOMEM;

  send_req->bufs = (uv_buf_t *)malloc(iovcnt * sizeof(uv_buf_t));
  send_req->buffers = (char **)malloc(iovcnt * sizeof(char *));
  if (!send_req->bufs || !send_req->buffers) {
    free(send_req->bufs);
    free(send_req->buffers);
    free(send_req);
    return UV_ENOMEM;
  }

  send_req->bufcnt = iovcnt;
  send_req->connection = connection;
  send_req->is_sendv = 1;
  for (size_t i = 0; i < iovcnt; i++) {
    send_req->bufs[i] = uv_buf_init((char *)iov[i].data, (unsigned int)iov[i].len);
    send_req->buffers[i] = (char *)iov[i].data;
  }
  send_req->req.data = send_req;

  int rc = uv_write(&send_req->req, (uv_stream_t *)&connection->handle.tcp, send_req->bufs,
                    (unsigned int)iovcnt, tcp_write_cb);
  if (rc != 0) {
    free(send_req->buffers);
    free(send_req->bufs);
    free(send_req);
  }
  return rc;
}

static int udp_sendv_impl(async_server_t *server, async_server_connection_t *connection,
                          const async_server_iovec_t *iov, size_t iovcnt) {
  async_server_udp_sendv_req_t *send_req =
      (async_server_udp_sendv_req_t *)calloc(1, sizeof(*send_req));
  if (!send_req)
    return UV_ENOMEM;

  send_req->bufs = (uv_buf_t *)malloc(iovcnt * sizeof(uv_buf_t));
  send_req->buffers = (char **)malloc(iovcnt * sizeof(char *));
  if (!send_req->bufs || !send_req->buffers) {
    free(send_req->bufs);
    free(send_req->buffers);
    free(send_req);
    return UV_ENOMEM;
  }

  send_req->bufcnt = iovcnt;
  send_req->is_sendv = 1;
  memcpy(&send_req->addr, &connection->handle.udp.addr, sizeof(send_req->addr));
  for (size_t i = 0; i < iovcnt; i++) {
    send_req->bufs[i] = uv_buf_init((char *)iov[i].data, (unsigned int)iov[i].len);
    send_req->buffers[i] = (char *)iov[i].data;
  }
  send_req->req.data = send_req;

  int rc = uv_udp_send(&send_req->req, &server->proto.udp.handle, send_req->bufs, (unsigned int)iovcnt,
                       (const struct sockaddr *)&send_req->addr, udp_send_cb);
  if (rc != 0) {
    free(send_req->buffers);
    free(send_req->bufs);
    free(send_req);
  }
  return rc;
}

static int kcp_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                          size_t iovcnt) {
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)connection->handle.kcp_peer;
  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_arena_wrap_external((void *)iov[i].data, iov[i].len, NULL, NULL);
      if (!buffer) {
        rc = UV_ENOMEM;
        break;
      }
      rc = turbo_kcp_send_buffer(kcp_client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

static int tls_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                          size_t iovcnt) {
  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_arena_wrap_external((void *)iov[i].data, iov[i].len, NULL, NULL);
      if (!buffer) {
        rc = UV_ENOMEM;
        break;
      }
      rc = turbo_tls_send_buffer(connection->handle.tls, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

static int pipe_sendv_impl(async_server_connection_t *connection, const async_server_iovec_t *iov,
                           size_t iovcnt) {
  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      turbo_arena_buffer_t *buffer =
          turbo_arena_wrap_external((void *)iov[i].data, iov[i].len, NULL, NULL);
      if (!buffer) {
        rc = UV_ENOMEM;
        break;
      }
      rc = turbo_pipe_send_buffer(connection->handle.pipe, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

static void handle_sendv_command(async_server_t *server, async_server_command_t *cmd) {
  async_server_connection_t *connection = cmd->payload.sendv.connection;
  async_server_iovec_t *iov = cmd->payload.sendv.iov;
  size_t iovcnt = cmd->payload.sendv.iovcnt;

  if (!connection || !connection->active || !iov || iovcnt == 0)
    return;

  cmd->payload.sendv.iov = NULL;

  size_t total_bytes = calculate_iov_total_bytes(iov, iovcnt);
  update_sendv_stats(server, connection, total_bytes, iovcnt);

  /* Dispatch via vtable - NO SWITCH! */
  int rc = server->ops->sendv(server, connection, iov, iovcnt);
  if (rc != 0)
    emit_uv_error(server, rc, server->ops->name);
}

static void handle_sendv_slices_command(async_server_t *server, async_server_command_t *cmd) {
  async_server_connection_t *connection = cmd->payload.sendv_slices.connection;
  turbo_arena_slice_t *slices = cmd->payload.sendv_slices.slices;
  size_t slice_count = cmd->payload.sendv_slices.slice_count;

  if (!connection || !connection->active || !slices || slice_count == 0)
    return;

  /* Calculate total bytes for stats */
  size_t total_bytes = 0;
  for (size_t i = 0; i < slice_count; i++) {
    total_bytes += slices[i].length;
  }

  update_sendv_stats(server, connection, total_bytes, slice_count);

  /* Convert arena slices to iovec format for transport layer */
  async_server_iovec_t *iov = (async_server_iovec_t *)malloc(slice_count * sizeof(async_server_iovec_t));
  if (!iov) {
    emit_error_message(server, -ASYNC_SERVER_STATUS_ALLOC_FAILED, "failed to allocate iov");
    return;
  }

  for (size_t i = 0; i < slice_count; i++) {
    iov[i].data = slices[i].data;
    iov[i].len = slices[i].length;
  }

  /* Dispatch via vtable - NO SWITCH! */
  int rc = server->ops->sendv(server, connection, iov, slice_count);

  free(iov);

  if (rc != 0)
    emit_uv_error(server, rc, server->ops->name);

  /* Transfer ownership - slices will be freed in free_command() after this returns */
  cmd->payload.sendv_slices.slices = NULL;
  cmd->payload.sendv_slices.slice_count = 0;
}

static void handle_broadcast_command(async_server_t *server, async_server_command_t *cmd) {
  char *data = cmd->payload.broadcast.data;
  size_t len = cmd->payload.broadcast.len;

  if (!data || len == 0) {
    free(data);
    return;
  }

  cmd->payload.broadcast.data = NULL;

  uv_mutex_lock(&server->mutex);
  server->stats.broadcasts++;
  uv_mutex_unlock(&server->mutex);

  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->active) {
      async_server_command_t *send_cmd = command_create_send(conn, data, len);
      if (send_cmd) {
        handle_send_command(server, send_cmd);
        free_command(send_cmd);
      }
    }
    conn = conn->next;
  }

  free(data);
}

static void handle_close_connection_command(async_server_t *server, async_server_command_t *cmd) {
  async_server_connection_t *connection = cmd->payload.close_connection.connection;
  if (!connection)
    return;

  connection->active = 0;
  connection->closing = 1;
  emit_disconnection(server, connection);
  connection_destroy(connection);
}

static void handle_stop_command(async_server_t *server) {
  /* Note: transport_close() is called in loop_thread_main() after loop exits.
   * Calling it here would destroy connections before the loop has a chance
   * to process close callbacks, causing clients to hang waiting for proper
   * connection close completion. */
  emit_closed(server);
  if (!uv_is_closing((uv_handle_t *)&server->command_async))
    uv_close((uv_handle_t *)&server->command_async, NULL);
  uv_stop(&server->loop);
}

static void tcp_connection_cb(uv_stream_t *server_handle, int status) {
  async_server_t *server = (async_server_t *)server_handle->data;

  if (status < 0) {
    emit_uv_error(server, status, CTX_TCP_CONNECTION);
    return;
  }

  if (server->max_connections > 0 && server->connection_count >= server->max_connections) {
    uv_mutex_lock(&server->mutex);
    server->stats.rejected_connections++;
    uv_mutex_unlock(&server->mutex);
    return;
  }

  async_server_connection_t *conn = connection_create(server);
  if (!conn)
    return;

  int rc = uv_tcp_init(&server->loop, &conn->handle.tcp);
  if (rc != 0) {
    free(conn);
    return;
  }

  conn->handle.tcp.data = conn;

  rc = uv_accept(server_handle, (uv_stream_t *)&conn->handle.tcp);
  if (rc != 0) {
    uv_close((uv_handle_t *)&conn->handle.tcp, NULL);
    free(conn);
    return;
  }

  struct sockaddr_storage addr;
  int addr_len = sizeof(addr);
  uv_tcp_getpeername(&conn->handle.tcp, (struct sockaddr *)&addr, &addr_len);
  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
    inet_ntop(AF_INET, &addr4->sin_addr, conn->remote_address, sizeof(conn->remote_address));
    conn->remote_port = ntohs(addr4->sin_port);
  } else if (addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
    inet_ntop(AF_INET6, &addr6->sin6_addr, conn->remote_address, sizeof(conn->remote_address));
    conn->remote_port = ntohs(addr6->sin6_port);
  }

  uv_tcp_getsockname(&conn->handle.tcp, (struct sockaddr *)&addr, &addr_len);
  if (addr.ss_family == AF_INET) {
    struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
    inet_ntop(AF_INET, &addr4->sin_addr, conn->local_address, sizeof(conn->local_address));
    conn->local_port = ntohs(addr4->sin_port);
  } else if (addr.ss_family == AF_INET6) {
    struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
    inet_ntop(AF_INET6, &addr6->sin6_addr, conn->local_address, sizeof(conn->local_address));
    conn->local_port = ntohs(addr6->sin6_port);
  }

  rc = uv_read_start((uv_stream_t *)&conn->handle.tcp, tcp_alloc_cb, tcp_read_cb);
  if (rc != 0) {
    uv_close((uv_handle_t *)&conn->handle.tcp, NULL);
    free(conn);
    return;
  }

  connection_add(server, conn);

  uv_mutex_lock(&server->mutex);
  server->stats.total_connections++;
  uv_mutex_unlock(&server->mutex);

  emit_connection(server, conn);
}

static void tcp_write_cb(uv_write_t *req, int status) {
  async_server_tcp_send_req_t *send_req = (async_server_tcp_send_req_t *)req->data;

  if (send_req) {
    if (send_req->is_sendv) {
      async_server_tcp_sendv_req_t *sendv_req = (async_server_tcp_sendv_req_t *)send_req;
      free(sendv_req->buffers);
      free(sendv_req->bufs);
      free(sendv_req);
    } else {
      free(send_req->buffer);
      free(send_req);
    }
  }

  if (status < 0) {
    async_server_connection_t *conn = (async_server_connection_t *)req->handle->data;
    if (conn && conn->server)
      emit_uv_error(conn->server, status, CTX_TCP_WRITE);
  }
}

static void tcp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  async_server_connection_t *conn = (async_server_connection_t *)handle->data;
  async_server_t *server = conn->server;

  if (suggested_size == 0)
    suggested_size = 4096;

  /* Use arena for zero-copy allocation */
  /* Layout: [arena_buffer_t*][actual data] */
  size_t total_size = sizeof(turbo_arena_buffer_t *) + suggested_size;
  turbo_arena_buffer_t *arena_buf = turbo_arena_get_buffer(&server->arena, total_size);
  if (!arena_buf) {
    /* Fallback to malloc if arena fails */
    char *base = (char *)malloc(suggested_size);
    if (!base) {
      buf->base = NULL;
      buf->len = 0;
      return;
    }
    buf->base = base;
    buf->len = (unsigned int)suggested_size;
    return;
  }

  /* Store arena buffer pointer at start, return data area */
  turbo_arena_buffer_t **header = (turbo_arena_buffer_t **)arena_buf->data;
  *header = arena_buf;
  buf->base = (char *)arena_buf->data + sizeof(turbo_arena_buffer_t *);
  buf->len = (unsigned int)suggested_size;
}

static void tcp_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  async_server_connection_t *conn = (async_server_connection_t *)stream->data;
  async_server_t *server = conn->server;

  if (!buf->base)
    return;

  /* Check if buffer is from arena (has header pointer) */
  turbo_arena_buffer_t **header_ptr = (turbo_arena_buffer_t **)(buf->base - sizeof(turbo_arena_buffer_t *));
  turbo_arena_buffer_t *arena_buf = NULL;

  /* Validate if this looks like an arena buffer by checking if header points to valid arena */
  if (header_ptr && *header_ptr && (*header_ptr)->arena == &server->arena) {
    arena_buf = *header_ptr;
  }

  if (nread == 0) {
    if (arena_buf)
      turbo_arena_buffer_unref(arena_buf);
    else
      free(buf->base);
    return;
  }

  if (nread < 0) {
    if (nread != UV_EOF)
      emit_uv_error(server, (int)nread, CTX_TCP_READ);
    if (arena_buf)
      turbo_arena_buffer_unref(arena_buf);
    else
      free(buf->base);
    conn->active = 0;
    emit_disconnection(server, conn);
    connection_destroy(conn);
    return;
  }

  uv_mutex_lock(&server->mutex);
  server->stats.messages_received++;
  server->stats.bytes_received += (size_t)nread;
  conn->bytes_received += (size_t)nread;
  uv_mutex_unlock(&server->mutex);

  if (arena_buf) {
    /* Zero-copy path: create slice and emit */
    turbo_arena_slice_t slice;
    slice.data = buf->base;
    slice.length = (size_t)nread;
    slice.buffer = arena_buf;
    emit_data_slice(server, conn, &slice, 1);
  } else {
    /* Malloc path: transfer ownership */
    emit_data(server, conn, buf->base, (size_t)nread, 1);
  }
}

static void tcp_close_cb(uv_handle_t *handle) {
  async_server_t *server = (async_server_t *)handle->data;
  emit_closed(server);
}

static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  async_server_t *server = (async_server_t *)handle->data;

  if (suggested_size == 0)
    suggested_size = 4096;

  /* Use arena for zero-copy allocation */
  /* Layout: [arena_buffer_t*][actual data] */
  size_t total_size = sizeof(turbo_arena_buffer_t *) + suggested_size;
  turbo_arena_buffer_t *arena_buf = turbo_arena_get_buffer(&server->arena, total_size);
  if (!arena_buf) {
    /* Fallback to malloc if arena fails */
    char *base = (char *)malloc(suggested_size);
    if (!base) {
      buf->base = NULL;
      buf->len = 0;
      return;
    }
    buf->base = base;
    buf->len = (unsigned int)suggested_size;
    return;
  }

  /* Store arena buffer pointer at start, return data area */
  turbo_arena_buffer_t **header = (turbo_arena_buffer_t **)arena_buf->data;
  *header = arena_buf;
  buf->base = (char *)arena_buf->data + sizeof(turbo_arena_buffer_t *);
  buf->len = (unsigned int)suggested_size;
}

/* Safely compare a sockaddr_storage with a sockaddr of unknown size (e.g. from libuv) */
static int safe_compare_sockaddr(const struct sockaddr_storage *stored, const struct sockaddr *incoming) {
  if (stored->ss_family != incoming->sa_family) return -1;
  if (incoming->sa_family == AF_INET6) {
    return memcmp(stored, incoming, sizeof(struct sockaddr_in6));
  } else {
    return memcmp(stored, incoming, sizeof(struct sockaddr_in));
  }
}

/* Safely copy a sockaddr of unknown size (e.g. from libuv) to a sockaddr_storage */
static void safe_copy_sockaddr(struct sockaddr_storage *dest, const struct sockaddr *src) {
  memset(dest, 0, sizeof(struct sockaddr_storage));
  if (src->sa_family == AF_INET6) {
    memcpy(dest, src, sizeof(struct sockaddr_in6));
  } else {
    memcpy(dest, src, sizeof(struct sockaddr_in));
  }
}


static void udp_recv_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
  UNUSED(flags);
  async_server_t *server = (async_server_t *)handle->data;

  if (!buf->base)
    return;

  /* Check if buffer is from arena (has header pointer) */
  turbo_arena_buffer_t **header_ptr = (turbo_arena_buffer_t **)(buf->base - sizeof(turbo_arena_buffer_t *));
  turbo_arena_buffer_t *arena_buf = NULL;

  /* Validate if this looks like an arena buffer by checking if header points to valid arena */
  if (header_ptr && *header_ptr && (*header_ptr)->arena == &server->arena) {
    arena_buf = *header_ptr;
  }

  if (nread <= 0) {
    if (nread < 0)
      emit_uv_error(server, (int)nread, CTX_UDP_RECV);
    if (arena_buf)
      turbo_arena_buffer_unref(arena_buf);
    else
      free(buf->base);
    return;
  }

  if (!addr) {
    if (arena_buf)
      turbo_arena_buffer_unref(arena_buf);
    else
      free(buf->base);
    return;
  }

  /* Find or create connection for this address */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (safe_compare_sockaddr(&conn->handle.udp.addr, addr) == 0)
      break;
    conn = conn->next;
  }

  if (!conn) {
    conn = connection_create(server);
    if (!conn) {
      if (arena_buf)
        turbo_arena_buffer_unref(arena_buf);
      else
        free(buf->base);
      return;
    }
    safe_copy_sockaddr(&conn->handle.udp.addr, addr);
    conn->handle.udp.addr_len =
        (addr->sa_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);

    if (addr->sa_family == AF_INET) {
      struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
      inet_ntop(AF_INET, &addr4->sin_addr, conn->remote_address, sizeof(conn->remote_address));
      conn->remote_port = ntohs(addr4->sin_port);
    } else if (addr->sa_family == AF_INET6) {
      struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
      inet_ntop(AF_INET6, &addr6->sin6_addr, conn->remote_address, sizeof(conn->remote_address));
      conn->remote_port = ntohs(addr6->sin6_port);
    }

    connection_add(server, conn);

    uv_mutex_lock(&server->mutex);
    server->stats.total_connections++;
    uv_mutex_unlock(&server->mutex);

    emit_connection(server, conn);
  }

  uv_mutex_lock(&server->mutex);
  server->stats.messages_received++;
  server->stats.bytes_received += (size_t)nread;
  conn->bytes_received += (size_t)nread;
  uv_mutex_unlock(&server->mutex);

  if (arena_buf) {
    /* Zero-copy path: create slice and emit */
    turbo_arena_slice_t slice;
    slice.data = buf->base;
    slice.length = (size_t)nread;
    slice.buffer = arena_buf;
    emit_data_slice(server, conn, &slice, 1);
  } else {
    /* Malloc path: transfer ownership */
    emit_data(server, conn, buf->base, (size_t)nread, 1);
  }
}

static void udp_send_cb(uv_udp_send_t *req, int status) {
  async_server_udp_send_req_t *send_req = (async_server_udp_send_req_t *)req->data;

  if (send_req) {
    if (send_req->is_sendv) {
      async_server_udp_sendv_req_t *sendv_req = (async_server_udp_sendv_req_t *)send_req;
      free(sendv_req->buffers);
      free(sendv_req->bufs);
      free(sendv_req);
    } else {
      free(send_req->buffer);
      free(send_req);
    }
  }

  if (status < 0) {
    /* UDP send error - no specific connection to report to */
  }
}

static int tls_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_tls_client_t *tls_client = (turbo_tls_client_t *)handle;
  if (!tls_client || !tls_client->server || !data || data->length == 0)
    return 0;

  async_server_t *server = server_from_tls_impl(tls_client->server);
  if (!server)
    return 0;

  /* Find connection for this TLS client */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->handle.tls == tls_client)
      break;
    conn = conn->next;
  }

  if (!conn)
    return 0;

  uv_mutex_lock(&server->mutex);
  server->stats.messages_received++;
  server->stats.bytes_received += data->length;
  conn->bytes_received += data->length;
  uv_mutex_unlock(&server->mutex);

  emit_data_slice(server, conn, data, 0);
  return 0;
}

static void tls_accept_cb(turbo_tls_server_t *tls_server, turbo_tls_client_t *client, void *peer) {
  UNUSED(peer);
  async_server_t *server = server_from_tls_impl(tls_server);
  if (!server)
    return;

  if (server->max_connections > 0 && server->connection_count >= server->max_connections) {
    uv_mutex_lock(&server->mutex);
    server->stats.rejected_connections++;
    uv_mutex_unlock(&server->mutex);
    turbo_tls_client_close(client);
    return;
  }

  async_server_connection_t *conn = connection_create(server);
  if (!conn) {
    turbo_tls_client_close(client);
    return;
  }

  conn->handle.tls = client;
  connection_add(server, conn);

  uv_mutex_lock(&server->mutex);
  server->stats.total_connections++;
  uv_mutex_unlock(&server->mutex);

  emit_connection(server, conn);
}

static void tls_close_cb(void *handle) {
  turbo_tls_client_t *tls_client = (turbo_tls_client_t *)handle;
  if (!tls_client || !tls_client->server)
    return;

  async_server_t *server = server_from_tls_impl(tls_client->server);
  if (!server)
    return;

  /* Find and remove connection */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->handle.tls == tls_client) {
      conn->active = 0;
      emit_disconnection(server, conn);
      connection_destroy(conn);
      break;
    }
    conn = conn->next;
  }
}

static int pipe_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_pipe_client_t *pipe_client = (turbo_pipe_client_t *)handle;
  if (!pipe_client || !pipe_client->server || !data || data->length == 0) {
    if (data)
      turbo_arena_slice_release((turbo_arena_slice_t *)data);
    return 0;
  }

  turbo_pipe_server_t *pipe_server = pipe_client->server;
  async_server_t *server = server_from_pipe(pipe_server);
  if (!server) {
    turbo_arena_slice_release((turbo_arena_slice_t *)data);
    return 0;
  }

  /* Find connection for this pipe client */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->handle.pipe == pipe_client)
      break;
    conn = conn->next;
  }

  if (!conn) {
    turbo_arena_slice_release((turbo_arena_slice_t *)data);
    return 0;
  }

  uv_mutex_lock(&server->mutex);
  server->stats.messages_received++;
  server->stats.bytes_received += data->length;
  conn->bytes_received += data->length;
  uv_mutex_unlock(&server->mutex);

  emit_data_slice(server, conn, data, 1);
  return 0;
}

static void pipe_accept_cb(void *handle, int status, void *peer) {
  /* turbo_connect_cb signature: (handle=client, status=0, peer=server) */
  UNUSED(status);
  turbo_pipe_client_t *client = (turbo_pipe_client_t *)handle;
  turbo_pipe_server_t *pipe_server = (turbo_pipe_server_t *)peer;

  if (!client || !pipe_server)
    return;

  async_server_t *server = server_from_pipe(pipe_server);
  if (!server)
    return;

  if (server->max_connections > 0 && server->connection_count >= server->max_connections) {
    uv_mutex_lock(&server->mutex);
    server->stats.rejected_connections++;
    uv_mutex_unlock(&server->mutex);
    turbo_pipe_client_close(client);
    return;
  }

  async_server_connection_t *conn = connection_create(server);
  if (!conn) {
    turbo_pipe_client_close(client);
    return;
  }

  conn->handle.pipe = client;
  connection_add(server, conn);

  uv_mutex_lock(&server->mutex);
  server->stats.total_connections++;
  uv_mutex_unlock(&server->mutex);

  emit_connection(server, conn);
}

static void pipe_close_cb(void *handle) {
  turbo_pipe_client_t *pipe_client = (turbo_pipe_client_t *)handle;
  if (!pipe_client || !pipe_client->server)
    return;

  async_server_t *server = server_from_pipe(pipe_client->server);
  if (!server)
    return;

  /* Find and remove connection */
  async_server_connection_t *conn = server->connections_head;
  while (conn) {
    if (conn->handle.pipe == pipe_client) {
      conn->active = 0;
      emit_disconnection(server, conn);
      connection_destroy(conn);
      break;
    }
    conn = conn->next;
  }
}

static void emit_event(async_server_t *server, async_server_event_type_t type,
                       async_server_connection_t *connection, const char *data, size_t length,
                       int status, const char *message, const turbo_arena_slice_t *slice,
                       async_server_event_flags_t flags) {
  if (!server->callback)
    return;

  async_server_event_t event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.connection = connection;
  event.data = data;
  event.length = length;
  event.status = status;
  event.slice = slice;
  event.flags = flags;

  if (message && message[0] != '\0') {
    size_t copy_len = sizeof(server->event_message) - 1;
    strncpy(server->event_message, message, copy_len);
    server->event_message[copy_len] = '\0';
    event.message = server->event_message;
  } else {
    server->event_message[0] = '\0';
    event.message = NULL;
  }

  server->callback(server, &event, server->callback_user_data);
}

static void emit_listening(async_server_t *server) {
  emit_event(server, ASYNC_SERVER_EVENT_LISTENING, NULL, NULL, 0, 0, NULL, NULL,
             ASYNC_SERVER_EVENT_FLAG_NONE);
}

static void emit_connection(async_server_t *server, async_server_connection_t *connection) {
  emit_event(server, ASYNC_SERVER_EVENT_CONNECTION, connection, NULL, 0, 0, NULL, NULL,
             ASYNC_SERVER_EVENT_FLAG_NONE);
}

static void emit_disconnection(async_server_t *server, async_server_connection_t *connection) {
  emit_event(server, ASYNC_SERVER_EVENT_DISCONNECTION, connection, NULL, 0, 0, NULL, NULL,
             ASYNC_SERVER_EVENT_FLAG_NONE);
}

static void emit_closed(async_server_t *server) {
  emit_event(server, ASYNC_SERVER_EVENT_CLOSED, NULL, NULL, 0, 0, NULL, NULL,
             ASYNC_SERVER_EVENT_FLAG_NONE);
}

static void emit_data(async_server_t *server, async_server_connection_t *connection, char *data,
                      size_t length, int free_after) {
  if (!server->callback || length == 0) {
    if (free_after && data)
      free(data);
    return;
  }

  emit_event(server, ASYNC_SERVER_EVENT_DATA, connection, data, length, 0, NULL, NULL,
             ASYNC_SERVER_EVENT_FLAG_NONE);

  if (free_after && data)
    free(data);
}

static void emit_data_slice(async_server_t *server, async_server_connection_t *connection,
                            const turbo_arena_slice_t *slice, int release_after) {
  if (!slice || slice->length == 0 || !server->callback) {
    if (release_after && slice)
      turbo_arena_slice_release((turbo_arena_slice_t *)slice);
    return;
  }

  emit_event(server, ASYNC_SERVER_EVENT_DATA, connection, (const char *)slice->data, slice->length,
             0, NULL, slice, ASYNC_SERVER_EVENT_FLAG_ZERO_COPY);

  if (release_after && slice)
    turbo_arena_slice_release((turbo_arena_slice_t *)slice);
}

static void emit_error_message(async_server_t *server, int status, const char *context) {
  uv_mutex_lock(&server->mutex);
  server->stats.send_errors++;
  uv_mutex_unlock(&server->mutex);

  char buffer[ASYNC_SERVER_ERROR_MESSAGE_MAX];
  if (context && context[0] != '\0') {
    strncpy(buffer, context, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
  } else
    buffer[0] = '\0';

  TLOG_ERROR("Async server error: {:s} (status: {:d})", buffer[0] ? buffer : "unknown", status);

  emit_event(server, ASYNC_SERVER_EVENT_ERROR, NULL, NULL, 0, status, buffer[0] ? buffer : NULL,
             NULL, ASYNC_SERVER_EVENT_FLAG_NONE);
}

static void emit_uv_error(async_server_t *server, int status, const char *context) {
  uv_mutex_lock(&server->mutex);
  if (context && (strstr(context, "send") || strstr(context, "write"))) {
    server->stats.send_errors++;
  } else if (context && (strstr(context, "recv") || strstr(context, "read"))) {
    server->stats.receive_errors++;
  }
  uv_mutex_unlock(&server->mutex);

  const char *uv_msg = uv_strerror(status);
  char buffer[ASYNC_SERVER_ERROR_MESSAGE_MAX];

  /* Copy to padded buffer to avoid ASan false positives from stb_sprintf 4-byte reads */
  char msg_padded[128] = {0};
  if (uv_msg) {
    strncpy(msg_padded, uv_msg, sizeof(msg_padded) - 1);
  } else {
    strncpy(msg_padded, "unknown", sizeof(msg_padded) - 1);
  }

  if (context && context[0] != '\0') {
    static const char FMT_UV_ERR_CTX[32] = "%s: %s (%d)";
    stbsp_snprintf(buffer, (int)sizeof(buffer), FMT_UV_ERR_CTX, context, msg_padded, status);
  } else {
    static const char FMT_UV_ERR[32] = "%s (%d)";
    stbsp_snprintf(buffer, (int)sizeof(buffer), FMT_UV_ERR, msg_padded, status);
  }

  TLOG_ERROR("Async server UV error: {:s}", buffer);

  emit_event(server, ASYNC_SERVER_EVENT_ERROR, NULL, NULL, 0, status, buffer, NULL,
             ASYNC_SERVER_EVENT_FLAG_NONE);
}

const char *async_server_status_to_string(async_server_status_t status) {
  switch (status) {
  case ASYNC_SERVER_STATUS_OK:
    return "ok";
  case ASYNC_SERVER_STATUS_INVALID_PARAM:
    return "invalid parameter";
  case ASYNC_SERVER_STATUS_ALLOC_FAILED:
    return "allocation failure";
  case ASYNC_SERVER_STATUS_NOT_READY:
    return "server not ready";
  case ASYNC_SERVER_STATUS_SHUTTING_DOWN:
    return "server shutting down";
  case ASYNC_SERVER_STATUS_IO_ERROR:
    return "I/O error";
  case ASYNC_SERVER_STATUS_TRANSPORT_ERROR:
    return "transport error";
  case ASYNC_SERVER_STATUS_INTERNAL_ERROR:
    return "internal error";
  default:
    return "unknown error";
  }
}

const char *async_server_transport_to_string(async_server_transport_t transport) {
  switch (transport) {
  case ASYNC_SERVER_TRANSPORT_TCP:
    return "tcp";
  case ASYNC_SERVER_TRANSPORT_UDP:
    return "udp";
  case ASYNC_SERVER_TRANSPORT_KCP:
    return "kcp";
  case ASYNC_SERVER_TRANSPORT_TLS:
    return "tls";
  case ASYNC_SERVER_TRANSPORT_PIPE:
    return "pipe";
  case ASYNC_SERVER_TRANSPORT_WEBSOCKET:
    return "websocket";
  default:
    return "unknown";
  }
}

async_server_t *async_server_create(async_server_transport_t transport,
                                    async_server_event_cb callback, void *user_data) {
  if (!callback)
    return NULL;

  async_server_t *server = (async_server_t *)calloc(1, sizeof(*server));
  if (!server)
    return NULL;

  server->transport = transport;

  /* Set transport operations vtable based on transport type */
  switch (transport) {
  case ASYNC_SERVER_TRANSPORT_TCP:
    server->ops = &server_tcp_ops;
    break;
  case ASYNC_SERVER_TRANSPORT_UDP:
    server->ops = &server_udp_ops;
    break;
  case ASYNC_SERVER_TRANSPORT_KCP:
    server->ops = &server_kcp_ops;
    break;
  case ASYNC_SERVER_TRANSPORT_TLS:
    server->ops = &server_tls_ops;
    break;
  case ASYNC_SERVER_TRANSPORT_PIPE:
    server->ops = &server_pipe_ops;
    break;
  case ASYNC_SERVER_TRANSPORT_WEBSOCKET:
    server->ops = &server_ws_ops;
    break;
  default:
    free(server);
    return NULL;
  }

  server->callback = callback;
  server->callback_user_data = user_data;

  if (uv_mutex_init(&server->mutex) != 0) {
    free(server);
    return NULL;
  }
  if (uv_cond_init(&server->cond) != 0) {
    uv_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
  }

  /* Initialize arena for zero-copy allocations */
  if (turbo_arena_init(&server->arena, 64 * 1024) != 0) {
    uv_cond_destroy(&server->cond);
    uv_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
  }

  if (client_common_config_acquire() == 0) {
    turbo_arena_free(&server->arena);
    uv_cond_destroy(&server->cond);
    uv_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
  }
  server->config_acquired = 1;

  if (dns_resolver_init() != 0) {
    client_common_config_release();
    server->config_acquired = 0;
    turbo_arena_free(&server->arena);
    uv_cond_destroy(&server->cond);
    uv_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
  }

  server_list_register(server);

  int rc = uv_thread_create(&server->loop_thread, loop_thread_main, server);
  if (rc != 0) {
    server_list_unregister(server);
    client_common_config_release();
    server->config_acquired = 0;
    turbo_arena_free(&server->arena);
    uv_cond_destroy(&server->cond);
    uv_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
  }

  uv_mutex_lock(&server->mutex);
  while (!server->loop_ready)
    uv_cond_wait(&server->cond, &server->mutex);
  rc = server->init_error;
  uv_mutex_unlock(&server->mutex);

  if (rc != 0) {
    uv_thread_join(&server->loop_thread);
    server_list_unregister(server);
    client_common_config_release();
    server->config_acquired = 0;
    turbo_arena_free(&server->arena);
    uv_cond_destroy(&server->cond);
    uv_mutex_destroy(&server->mutex);
    free(server);
    return NULL;
  }

  return server;
}

void async_server_destroy(async_server_t *server) {
  if (!server)
    return;

  async_server_stop(server);

  uv_thread_join(&server->loop_thread);

  server_list_unregister(server);

  dns_resolver_cleanup();

  turbo_arena_free(&server->arena);
  uv_cond_destroy(&server->cond);
  uv_mutex_destroy(&server->mutex);
  free(server);
}

async_server_status_t async_server_listen(async_server_t *server, const char *host, int port,
                                          int backlog) {
  if (!server)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  async_server_command_t *cmd = command_create_listen(host, port, backlog);
  if (!cmd)
    return ASYNC_SERVER_STATUS_ALLOC_FAILED;

  async_server_status_t status = submit_command(server, cmd, 0);
  if (status != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
  return status;
}

async_server_status_t async_server_send(async_server_t *server,
                                        async_server_connection_t *connection, const char *data,
                                        size_t len) {
  if (!server || !connection || (!data && len > 0))
    return ASYNC_SERVER_STATUS_INVALID_PARAM;
  if (len == 0)
    return ASYNC_SERVER_STATUS_OK;

  async_server_command_t *cmd = command_create_send(connection, data, len);
  if (!cmd)
    return ASYNC_SERVER_STATUS_ALLOC_FAILED;

  async_server_status_t status = submit_command(server, cmd, 0);
  if (status != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
  return status;
}

async_server_status_t async_server_sendv(async_server_t *server,
                                         async_server_connection_t *connection,
                                         const async_server_iovec_t *iov, size_t iovcnt) {
  if (!server || !connection || !iov || iovcnt == 0)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  async_server_command_t *cmd = command_create_sendv(connection, iov, iovcnt);
  if (!cmd)
    return ASYNC_SERVER_STATUS_ALLOC_FAILED;

  async_server_status_t status = submit_command(server, cmd, 0);
  if (status != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
  return status;
}

async_server_status_t async_server_sendv_slices(async_server_t *server,
                                                 async_server_connection_t *connection,
                                                 const turbo_arena_slice_t *slices,
                                                 size_t slice_count) {
  if (!server || !connection || !slices || slice_count == 0)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  /* Validate that all slices have buffers (safety check) */
  for (size_t i = 0; i < slice_count; i++) {
    if (!slices[i].buffer || !slices[i].data) {
      return ASYNC_SERVER_STATUS_INVALID_PARAM;
    }
  }

  async_server_command_t *cmd = command_create_sendv_slices(connection, slices, slice_count);
  if (!cmd)
    return ASYNC_SERVER_STATUS_ALLOC_FAILED;

  async_server_status_t status = submit_command(server, cmd, 0);
  if (status != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
  return status;
}

async_server_status_t async_server_send_to(async_server_t *server,
                                        async_server_connection_t *connection, const char *data,
                                        size_t len) {
  if (!server || !connection || (!data && len > 0))
    return ASYNC_SERVER_STATUS_INVALID_PARAM;
  if (len == 0)
    return ASYNC_SERVER_STATUS_OK;

  async_server_command_t *cmd = command_create_send(connection, data, len);
  if (!cmd)
    return ASYNC_SERVER_STATUS_ALLOC_FAILED;

  async_server_status_t status = submit_command(server, cmd, 0);
  if (status != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
  return status;
}

async_server_status_t async_server_broadcast(async_server_t *server, const char *data, size_t len) {
  if (!server || (!data && len > 0))
    return ASYNC_SERVER_STATUS_INVALID_PARAM;
  if (len == 0)
    return ASYNC_SERVER_STATUS_OK;

  async_server_command_t *cmd = command_create_broadcast(data, len);
  if (!cmd)
    return ASYNC_SERVER_STATUS_ALLOC_FAILED;

  async_server_status_t status = submit_command(server, cmd, 0);
  if (status != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
  return status;
}

void async_server_close_connection(async_server_t *server, async_server_connection_t *connection) {
  if (!server || !connection)
    return;

  async_server_command_t *cmd = command_create_close_connection(connection);
  if (!cmd)
    return;
  if (submit_command(server, cmd, 0) != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
}

void async_server_stop(async_server_t *server) {
  if (!server)
    return;

  uv_mutex_lock(&server->mutex);
  if (server->shutting_down) {
    uv_mutex_unlock(&server->mutex);
    return;
  }
  uv_mutex_unlock(&server->mutex);

  async_server_command_t *cmd = command_create_stop();
  if (!cmd)
    return;
  if (submit_command(server, cmd, 1) != ASYNC_SERVER_STATUS_OK)
    free_command(cmd);
}

async_server_state_t async_server_get_state(const async_server_t *server) {
  if (!server)
    return ASYNC_SERVER_STATE_STOPPED;

  if (server->shutting_down)
    return ASYNC_SERVER_STATE_STOPPING;

  if (!server->loop_running)
    return ASYNC_SERVER_STATE_STOPPED;

  if (server->listening)
    return ASYNC_SERVER_STATE_LISTENING;

  return ASYNC_SERVER_STATE_STOPPED;
}

int async_server_is_listening(const async_server_t *server) {
  return async_server_get_state(server) == ASYNC_SERVER_STATE_LISTENING;
}

size_t async_server_get_connection_count(const async_server_t *server) {
  if (!server)
    return 0;

  size_t count;
  uv_mutex_lock((uv_mutex_t *)&server->mutex);
  count = server->connection_count;
  uv_mutex_unlock((uv_mutex_t *)&server->mutex);
  return count;
}

void async_server_set_max_connections(async_server_t *server, size_t max_connections) {
  if (!server)
    return;

  uv_mutex_lock(&server->mutex);
  server->max_connections = max_connections;
  uv_mutex_unlock(&server->mutex);
}

void async_server_set_idle_timeout(async_server_t *server, int timeout_ms) {
  if (!server)
    return;

  uv_mutex_lock(&server->mutex);
  server->idle_timeout_ms = timeout_ms;
  uv_mutex_unlock(&server->mutex);
}

async_server_status_t async_server_get_connection_info(const async_server_connection_t *connection,
                                                       async_server_connection_info_t *info) {
  if (!connection || !info)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  strncpy(info->remote_address, connection->remote_address, sizeof(info->remote_address) - 1);
  info->remote_address[sizeof(info->remote_address) - 1] = '\0';
  info->remote_port = connection->remote_port;

  strncpy(info->local_address, connection->local_address, sizeof(info->local_address) - 1);
  info->local_address[sizeof(info->local_address) - 1] = '\0';
  info->local_port = connection->local_port;

  info->bytes_sent = connection->bytes_sent;
  info->bytes_received = connection->bytes_received;
  info->connect_time = connection->connect_time;

  return ASYNC_SERVER_STATUS_OK;
}

void async_server_connection_set_user_data(async_server_connection_t *connection, void *user_data) {
  if (!connection)
    return;
  connection->user_data = user_data;
}

void *async_server_connection_get_user_data(const async_server_connection_t *connection) {
  if (!connection)
    return NULL;
  return connection->user_data;
}

void async_server_get_stats(const async_server_t *server, async_server_stats_t *stats) {
  if (!server || !stats)
    return;

  uv_mutex_lock((uv_mutex_t *)&server->mutex);
  memcpy(stats, &server->stats, sizeof(async_server_stats_t));
  uv_mutex_unlock((uv_mutex_t *)&server->mutex);
}

void async_server_reset_stats(async_server_t *server) {
  if (!server)
    return;

  uv_mutex_lock(&server->mutex);
  memset(&server->stats, 0, sizeof(async_server_stats_t));
  uv_mutex_unlock(&server->mutex);
}

async_server_status_t async_server_set_tls_config(async_server_t *server,
                                                  const async_server_tls_config_t *config) {
  if (!server || !config)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  if (server->transport != ASYNC_SERVER_TRANSPORT_TLS)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  if (server->listening)
    return ASYNC_SERVER_STATUS_NOT_READY;

  /* Store config */
  memcpy(&server->proto.tls.config, config, sizeof(async_server_tls_config_t));

  /* Initialize TLS context */
  if (server->proto.tls.context_initialized) {
    turbo_tls_context_destroy(&server->proto.tls.context);
    server->proto.tls.context_initialized = 0;
  }

  int rc = turbo_tls_context_init(&server->proto.tls.context, TURBO_TLS_CONTEXT_LIB_INIT);
  if (rc != 0)
    return ASYNC_SERVER_STATUS_INTERNAL_ERROR;

  server->proto.tls.context_initialized = 1;

  /* Load certificate and key - using the available API */
  if (config->cert_file && config->key_file) {
    /* Read cert file */
    FILE *cert_fp = fopen(config->cert_file, "rb");
    if (!cert_fp) {
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_IO_ERROR;
    }
    fseek(cert_fp, 0, SEEK_END);
    long cert_size = ftell(cert_fp);
    fseek(cert_fp, 0, SEEK_SET);
    char *cert_data = (char *)malloc(cert_size + 1);
    if (!cert_data) {
      fclose(cert_fp);
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_ALLOC_FAILED;
    }
    fread(cert_data, 1, cert_size, cert_fp);
    cert_data[cert_size] = '\0';
    fclose(cert_fp);

    /* Read key file */
    FILE *key_fp = fopen(config->key_file, "rb");
    if (!key_fp) {
      free(cert_data);
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_IO_ERROR;
    }
    fseek(key_fp, 0, SEEK_END);
    long key_size = ftell(key_fp);
    fseek(key_fp, 0, SEEK_SET);
    char *key_data = (char *)malloc(key_size + 1);
    if (!key_data) {
      fclose(key_fp);
      free(cert_data);
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_ALLOC_FAILED;
    }
    fread(key_data, 1, key_size, key_fp);
    key_data[key_size] = '\0';
    fclose(key_fp);

    rc = turbo_tls_context_set_cert(&server->proto.tls.context, cert_data, cert_size);
    if (rc == 0) {
      rc = turbo_tls_context_set_private_key(&server->proto.tls.context, key_data, key_size);
    }

    free(cert_data);
    free(key_data);

    if (rc != 0) {
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_TRANSPORT_ERROR;
    }
  }

  /* Load CA if provided */
  if (config->ca_file) {
    FILE *ca_fp = fopen(config->ca_file, "rb");
    if (!ca_fp) {
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_IO_ERROR;
    }
    fseek(ca_fp, 0, SEEK_END);
    long ca_size = ftell(ca_fp);
    fseek(ca_fp, 0, SEEK_SET);
    char *ca_data = (char *)malloc(ca_size + 1);
    if (!ca_data) {
      fclose(ca_fp);
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_ALLOC_FAILED;
    }
    fread(ca_data, 1, ca_size, ca_fp);
    ca_data[ca_size] = '\0';
    fclose(ca_fp);

    rc = turbo_tls_context_add_trusted_certs(&server->proto.tls.context, ca_data, ca_size);
    free(ca_data);

    if (rc != 0) {
      turbo_tls_context_destroy(&server->proto.tls.context);
      server->proto.tls.context_initialized = 0;
      return ASYNC_SERVER_STATUS_TRANSPORT_ERROR;
    }
  }

  /* Set verification mode */
  if (config->verify_peer) {
    turbo_tls_context_set_verify_flags(&server->proto.tls.context, TURBO_TLS_VERIFY_PEER_CERT);
  } else {
    turbo_tls_context_set_verify_flags(&server->proto.tls.context, TURBO_TLS_VERIFY_NONE);
  }

  return ASYNC_SERVER_STATUS_OK;
}

/**
 * @brief Retrieves the underlying transport handle from an async_server_connection_t.
 *
 * @param connection A pointer to the async_server_connection_t.
 * @return A pointer to the transport-specific handle (e.g., uv_tcp_t for TCP), or NULL on error.
 */
void *async_server_connection_get_handle(async_server_connection_t *connection) {
  if (!connection)
    return NULL;
  return &connection->handle.tcp;
}

/**
 * @brief Adopts an existing transport handle into a new async_server_connection_t.
 *
 * This function is used by worker processes to take ownership of a handle
 * passed from a master process, integrating it into the async_server
 * event loop and connection management.
 *
 * @param server A pointer to the async_server_t instance (worker's server).
 * @param client_handle An already connected and accepted transport handle.
 * @param callback The event callback for this new connection.
 * @param user_data User-defined data to associate with the connection.
 * @return A pointer to the newly created async_server_connection_t, or NULL on failure.
 */
async_server_connection_t *async_server_adopt_handle(async_server_t *server,
                                                      void *client_handle,
                                                      async_server_event_cb callback,
                                                      void *user_data) {
  UNUSED(server);
  UNUSED(client_handle);
  UNUSED(callback);
  UNUSED(user_data);
  /* TODO: Implement if needed by workers */
  return NULL;
}

/**
 * @brief Join a UDP multicast group (UDP servers only).
 */
async_server_status_t async_server_join_multicast_group(async_server_t *server,
                                                        const char *multicast_addr,
                                                        const char *interface_addr) {
  if (!server || !multicast_addr)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  /* Use vtable - no protocol type checking! */
  if (!server->ops->join_multicast_group)
    return ASYNC_SERVER_STATUS_TRANSPORT_ERROR; /* Not supported by this transport */

  return server->ops->join_multicast_group(server, multicast_addr, interface_addr);
}

/**
 * @brief Leave a UDP multicast group (UDP servers only).
 */
async_server_status_t async_server_leave_multicast_group(async_server_t *server,
                                                         const char *multicast_addr,
                                                         const char *interface_addr) {
  if (!server || !multicast_addr)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  /* Use vtable - no protocol type checking! */
  if (!server->ops->leave_multicast_group)
    return ASYNC_SERVER_STATUS_TRANSPORT_ERROR; /* Not supported by this transport */

  return server->ops->leave_multicast_group(server, multicast_addr, interface_addr);
}

/**
 * @brief Set multicast TTL (UDP servers only).
 */
async_server_status_t async_server_set_multicast_ttl(async_server_t *server, int ttl) {
  if (!server)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  /* Use vtable - no protocol type checking! */
  if (!server->ops->set_multicast_ttl)
    return ASYNC_SERVER_STATUS_TRANSPORT_ERROR; /* Not supported by this transport */

  return server->ops->set_multicast_ttl(server, ttl);
}

/**
 * @brief Enable/disable multicast loopback (UDP servers only).
 */
async_server_status_t async_server_set_multicast_loop(async_server_t *server, int on) {
  if (!server)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  /* Use vtable - no protocol type checking! */
  if (!server->ops->set_multicast_loop)
    return ASYNC_SERVER_STATUS_TRANSPORT_ERROR; /* Not supported by this transport */

  return server->ops->set_multicast_loop(server, on);
}

/* ========================================================================
 * WebSocket vtable implementations
 * ======================================================================== */

static void ws_server_on_connection(void *handle, int status, void *peer);
static int ws_server_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer);
static void ws_server_on_close(void *handle);

static async_server_status_t server_ws_setup_impl(async_server_t *server) {
  server->proto.ws.server = NULL;
  server->proto.ws.config_set = 0;
  server->proto.ws.use_tls = 0;
  memset(&server->proto.ws.config, 0, sizeof(server->proto.ws.config));
  return ASYNC_SERVER_STATUS_OK;
}

static void ws_tcp_server_close_cb(uv_handle_t *handle) {
  /* The tcp server struct was stored in handle->data by turbo_tcp_server_init */
  turbo_tcp_server_t *tcp = (turbo_tcp_server_t *)handle->data;
  if (tcp) {
    turbo_arena_free(&tcp->arena);
    free(tcp->handle);
    tcp->handle = NULL;
    free(tcp);
  }
}

static void server_ws_close_impl(async_server_t *server) {
  if (server->proto.ws.server) {
    turbo_websocket_server_t *ws = server->proto.ws.server;

    /* Close the underlying TCP/TLS server handle properly via uv_close */
    if (ws->transport_server) {
      if (ws->is_tls) {
        turbo_tls_server_t *tls = (turbo_tls_server_t *)ws->transport_server;
        /* TLS server cleanup - let uv_close handle it */
        turbo_tls_server_stop(tls);
        free(tls);
      } else {
        turbo_tcp_server_t *tcp = (turbo_tcp_server_t *)ws->transport_server;
        /* Close handle and defer cleanup to callback to avoid use-after-free */
        if (tcp->handle && !uv_is_closing((uv_handle_t *)tcp->handle)) {
          tcp->handle->data = tcp; /* Store tcp for cleanup in callback */
          uv_close((uv_handle_t *)tcp->handle, ws_tcp_server_close_cb);
        } else {
          /* Handle already closing or NULL, clean up now */
          turbo_arena_free(&tcp->arena);
          if (tcp->handle) free(tcp->handle);
          free(tcp);
        }
      }
      ws->transport_server = NULL;
    }

    turbo_websocket_server_stop(ws);
    turbo_websocket_server_destroy(ws);
    server->proto.ws.server = NULL;
  }
  emit_closed(server);
}

static int server_ws_listen_impl(async_server_t *server, const char *host, int port, int backlog) {
  (void)backlog;

  if (!server->proto.ws.config_set) {
    emit_error_message(server, -1, "WebSocket config not set");
    return -1;
  }

  turbo_websocket_server_t *ws = turbo_websocket_server_create(
      &server->loop, server->proto.ws.use_tls, &server->proto.ws.config);
  if (!ws) {
    emit_error_message(server, -1, "Failed to create WebSocket server");
    return -1;
  }

  server->proto.ws.server = ws;
  ws->user_data = server;

  turbo_websocket_server_set_callbacks(ws, ws_server_on_connection, ws_server_on_recv, ws_server_on_close);

  int rc = turbo_websocket_server_listen(ws, host, port, backlog);
  if (rc != 0) {
    turbo_websocket_server_destroy(ws);
    server->proto.ws.server = NULL;
    emit_error_message(server, rc, "WebSocket listen failed");
    return rc;
  }

  server->listening = 1;
  /* Note: handle_listen_command calls emit_listening on success */
  return 0;
}

static int server_ws_send_impl(async_server_t *server, async_server_connection_t *conn, char *data, size_t len) {
  (void)server;

  if (!conn || !conn->handle.ws_conn) {
    free(data);
    return -1;
  }

  int rc = turbo_websocket_server_send(conn->handle.ws_conn, data, len);
  free(data);

  return rc;
}

static int server_ws_sendv_impl(async_server_t *server, async_server_connection_t *conn,
                                const async_server_iovec_t *iov, size_t iovcnt) {
  (void)server;

  if (!conn || !conn->handle.ws_conn || !iov || iovcnt == 0)
    return -1;

  size_t total_len = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    total_len += iov[i].len;
  }

  char *combined = malloc(total_len);
  if (!combined)
    return -1;

  size_t offset = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    memcpy(combined + offset, iov[i].data, iov[i].len);
    offset += iov[i].len;
  }

  int rc = turbo_websocket_server_send(conn->handle.ws_conn, combined, total_len);
  free(combined);

  return rc;
}

static void ws_server_on_connection(void *handle, int status, void *peer) {
  (void)handle;
  turbo_websocket_connection_t *ws_conn = (turbo_websocket_connection_t *)peer;
  if (!ws_conn || status != 0)
    return;

  turbo_websocket_server_t *ws_server = ws_conn->server;
  if (!ws_server)
    return;

  async_server_t *server = (async_server_t *)ws_server->user_data;
  if (!server)
    return;

  async_server_connection_t *conn = calloc(1, sizeof(*conn));
  if (!conn)
    return;

  conn->server = server;
  conn->handle.ws_conn = ws_conn;
  ws_conn->user_data = conn;

  uv_mutex_lock(&server->mutex);
  server->stats.total_connections++;
  server->stats.active_connections++;
  uv_mutex_unlock(&server->mutex);

  async_server_event_t event = {
      .type = ASYNC_SERVER_EVENT_CONNECTION,
      .connection = conn,
      .status = 0
  };

  if (server->callback) {
    server->callback(server, &event, server->callback_user_data);
  }
}

static int ws_server_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer) {
  (void)handle;
  turbo_websocket_connection_t *ws_conn = (turbo_websocket_connection_t *)peer;
  if (!ws_conn || !data)
    return 0;

  async_server_connection_t *conn = (async_server_connection_t *)ws_conn->user_data;
  if (!conn)
    return 0;

  async_server_t *server = conn->server;
  if (!server)
    return 0;

  uv_mutex_lock(&server->mutex);
  server->stats.bytes_received += data->length;
  server->stats.messages_received++;
  uv_mutex_unlock(&server->mutex);

  async_server_event_t event = {
      .type = ASYNC_SERVER_EVENT_DATA,
      .connection = conn,
      .slice = data,
      .length = data->length,
      .flags = ASYNC_SERVER_EVENT_FLAG_ZERO_COPY
  };

  if (server->callback) {
    server->callback(server, &event, server->callback_user_data);
  }

  return 0;
}

static void ws_server_on_close(void *handle) {
  turbo_websocket_connection_t *ws_conn = (turbo_websocket_connection_t *)handle;
  if (!ws_conn)
    return;

  async_server_connection_t *conn = (async_server_connection_t *)ws_conn->user_data;
  if (!conn)
    return;

  async_server_t *server = conn->server;
  if (!server)
    return;

  uv_mutex_lock(&server->mutex);
  if (server->stats.active_connections > 0)
    server->stats.active_connections--;
  uv_mutex_unlock(&server->mutex);

  async_server_event_t event = {
      .type = ASYNC_SERVER_EVENT_DISCONNECTION,
      .connection = conn
  };

  if (server->callback) {
    server->callback(server, &event, server->callback_user_data);
  }

  free(conn);
}

async_server_status_t async_server_set_ws_config(async_server_t *server,
                                                 const async_server_ws_config_t *config) {
  if (!server || !config)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  if (server->transport != ASYNC_SERVER_TRANSPORT_WEBSOCKET)
    return ASYNC_SERVER_STATUS_INVALID_PARAM;

  uv_mutex_lock(&server->mutex);

  /* Initialize turbo_websocket_server_config_t with sensible defaults */
  server->proto.ws.config.supported_subprotocols = NULL;
  server->proto.ws.config.subprotocol_count = 0;
  server->proto.ws.config.max_connections = 100;
  server->proto.ws.config.max_message_size = 1024 * 1024; /* 1MB */
  server->proto.ws.config.handshake_timeout_ms = 10000;   /* 10 seconds */
  server->proto.ws.use_tls = config->use_tls;
  server->proto.ws.config_set = 1;

  uv_mutex_unlock(&server->mutex);

  return ASYNC_SERVER_STATUS_OK;
}
