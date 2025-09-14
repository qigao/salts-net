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
#include "turbo_async_client.h"
#include "turbo_dns.h"
#include "turbo_kcp.h"
#include "turbo_pipe.h"
#include "turbo_tcp.h"
#include "turbo_tls.h"
#include "turbo_websocket_client.h"

#define ASYNC_CLIENT_ERROR_MESSAGE_MAX 128
#define UNUSED(x) (void)(x)

#ifndef CONTAINER_OF
  #define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

typedef enum {
  COMMAND_CONNECT,
  COMMAND_SEND,
  COMMAND_SENDV,
  COMMAND_SENDV_SLICES, /* NEW: True zero-copy with arena slices */
  COMMAND_CLOSE,
  COMMAND_STOP
} async_client_command_type_t;

typedef struct async_client_command_s {
  async_client_command_type_t type;
  union {
    struct {
      char *host;
      int port;
    } connect;
    struct {
      char *data;
      size_t len;
    } send;
    struct {
      async_client_iovec_t *iov;
      size_t iovcnt;
    } sendv;
    struct {
      turbo_arena_slice_t *slices; /* Arena slices with refcount */
      size_t slice_count;
    } sendv_slices;
  } payload;
  struct async_client_command_s *next;
} async_client_command_t;

/* Forward declarations for static helper functions and callbacks */
static async_client_status_t transport_setup(async_client_t *client);
static void transport_close(async_client_t *client);

static void handle_connect_command(async_client_t *client, async_client_command_t *cmd);
static void handle_send_command(async_client_t *client, async_client_command_t *cmd);
static void handle_sendv_command(async_client_t *client, async_client_command_t *cmd);
static void handle_sendv_slices_command(async_client_t *client, async_client_command_t *cmd);
static void handle_close_command(async_client_t *client);

/* Phase 6a: Helper functions for handle_sendv_command refactoring */
static size_t client_calculate_iov_total_bytes(const async_client_iovec_t *iov, size_t iovcnt);
static void client_update_sendv_stats(async_client_t *client, size_t total_bytes, size_t iovcnt);
static int client_tcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                 size_t iovcnt);
static int client_udp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                 size_t iovcnt);
static int client_kcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                 size_t iovcnt);
static int client_tls_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                 size_t iovcnt);
static int client_pipe_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                  size_t iovcnt);

/* Phase 6b: Helper functions for handle_connect_command refactoring */
static void client_start_connect_timeout(async_client_t *client);
static int client_tcp_connect_impl(async_client_t *client, const char *host, int port);
static int client_udp_connect_impl(async_client_t *client, const char *host, int port);
static int client_kcp_connect_impl(async_client_t *client, const char *host, int port);
static int client_tls_connect_impl(async_client_t *client, const char *host, int port);
static int client_pipe_connect_impl(async_client_t *client, const char *host);
static void handle_stop_command(async_client_t *client);

static void tcp_connect_cb(uv_connect_t *req, int status);
static void tcp_write_cb(uv_write_t *req, int status);
static void tcp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void tcp_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void tcp_close_cb(uv_handle_t *handle);

static void udp_send_cb(uv_udp_send_t *req, int status);
static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void udp_read_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags);
static void udp_close_cb(uv_handle_t *handle);

static void kcp_connect_cb(turbo_kcp_client_t *kcp_client, int status, void *peer);
static int kcp_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);

static void tls_connect_cb(turbo_tls_client_t *tls_client, int status, void *peer);
static void tls_close_cb(void *handle);
static void tls_handshake_cb(turbo_tls_client_t *tls_client, int status);
static int tls_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);

static int pipe_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer);
static void pipe_connect_cb(turbo_pipe_client_t *pipe_client, int status, void *peer);
static void pipe_close_cb(void *handle);

static void emit_event(async_client_t *client, async_client_event_type_t type, const char *data,
                       size_t length, int status, const char *message,
                       const turbo_arena_slice_t *slice, async_client_event_flags_t flags);
static void emit_connected(async_client_t *client);
static void emit_closed(async_client_t *client);
static void emit_data(async_client_t *client, char *data, size_t length, int free_after);
static void emit_data_slice(async_client_t *client, const turbo_arena_slice_t *slice,
                            int release_after);
static void emit_error_message(async_client_t *client, int status, const char *context);
static void emit_uv_error(async_client_t *client, int status, const char *context);

static void connect_timeout_cb(uv_timer_t *timer);
static void operation_timeout_cb(uv_timer_t *timer);
static void start_operation_timer(async_client_t *client);

typedef struct {
  uv_tcp_t handle;
  uv_connect_t connect_req;
  int active;
} async_client_tcp_state_t;

typedef struct async_client_tcp_send_req_s {
  uv_write_t req;
  char *buffer;
  size_t length;
  int is_sendv; /* 0 for regular send, 1 for sendv */
} async_client_tcp_send_req_t;

typedef struct async_client_tcp_sendv_req_s {
  uv_write_t req;
  uv_buf_t *bufs;
  size_t bufcnt;
  char **buffers; /* Array of buffer pointers for cleanup */
  int is_sendv;   /* Always 1 */
} async_client_tcp_sendv_req_t;

typedef struct {
  uv_udp_t handle;
  struct sockaddr_storage remote_addr;
  int remote_addr_len;
  int connected;
  int recv_active;
} async_client_udp_state_t;

typedef struct async_client_udp_send_req_s {
  uv_udp_send_t req;
  char *buffer;
  size_t length;
  int is_sendv; /* 0 for regular send, 1 for sendv */
} async_client_udp_send_req_t;

typedef struct async_client_udp_sendv_req_s {
  uv_udp_send_t req;
  uv_buf_t *bufs;
  size_t bufcnt;
  char **buffers; /* Array of buffer pointers for cleanup */
  int is_sendv;   /* Always 1 */
} async_client_udp_sendv_req_t;

typedef struct {
  turbo_kcp_client_t client;
  int initialized;
} async_client_kcp_state_t;

typedef struct {
  turbo_tls_context_t context;
  turbo_tls_client_t *client;
  int context_initialized;
  int client_created;
  int handshake_pending;
} async_client_tls_state_t;

typedef struct {
  turbo_pipe_client_t *client;
} async_client_pipe_state_t;

typedef struct {
  turbo_websocket_client_t *client;
  turbo_websocket_config_t config;
  int use_tls;
  int config_set;
} async_client_ws_state_t;

/* ========================================================================
 * Transport operations vtable - eliminates all protocol switch statements
 *
 * Each protocol (TCP/UDP/KCP/TLS/PIPE) implements this interface.
 * Main code calls client->ops->xxx() without knowing which protocol.
 *
 * This is "good taste" - no special cases, just polymorphism.
 * ======================================================================== */

typedef struct async_client_s async_client_t;

typedef struct async_client_transport_ops_s {
  /* Initialize transport-specific state (called during loop setup) */
  async_client_status_t (*setup)(async_client_t *client);

  /* Close transport-specific handles */
  void (*close)(async_client_t *client);

  /* Connect to remote host:port */
  int (*connect)(async_client_t *client, const char *host, int port);

  /* Send single buffer (data ownership transferred) */
  int (*send)(async_client_t *client, char *data, size_t len);

  /* Send scatter-gather IOV array */
  int (*sendv)(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);

  /* Get connection state */
  async_client_state_t (*get_state)(const async_client_t *client);

  /* Optional protocol-specific methods (NULL if not supported) */
  async_client_status_t (*set_multicast_ttl)(async_client_t *client, int ttl);
  async_client_status_t (*set_multicast_loop)(async_client_t *client, int on);

  /* Protocol name for debugging */
  const char *name;
} async_client_transport_ops_t;

struct async_client_s {
  uv_loop_t loop;
  uv_thread_t loop_thread;
  uv_async_t command_async;

  uv_mutex_t mutex;
  uv_cond_t cond;

  async_client_command_t *command_head;
  async_client_command_t *command_tail;

  async_client_transport_t transport;
  const async_client_transport_ops_t *ops; /* Vtable - no more switch! */

  /* Phase 3: Union optimization - only one protocol active at a time */
  union {
    async_client_tcp_state_t tcp;
    async_client_udp_state_t udp;
    async_client_kcp_state_t kcp;
    async_client_tls_state_t tls;
    async_client_pipe_state_t pipe;
    async_client_ws_state_t ws;
  } proto;

  async_client_event_cb callback;
  void *callback_user_data;

  int loop_ready;
  int loop_running;
  int shutting_down;
  int init_error;

  int close_requested;
  int closed_emitted;
  int config_acquired;

  char event_message[ASYNC_CLIENT_ERROR_MESSAGE_MAX];

  /* Timeout support */
  int connect_timeout_ms;
  int operation_timeout_ms;
  uv_timer_t connect_timer;
  uv_timer_t operation_timer;
  int connect_timer_active;
  int operation_timer_active;

  /* Statistics */
  async_client_stats_t stats;

  /* Zero-copy arena memory management */
  turbo_arena_t arena;

  struct async_client_s *global_next;
};

CLIENT_COMMON_DEFINE_PIPE_CLIENT_LIST(async, async_client_t);
#define g_client_list_once g_async_client_list_once
#define g_client_list_lock g_async_client_list_lock
#define g_client_list_initialized g_async_client_list_initialized
#define g_client_list_head g_async_client_list_head

/**
 * @brief Initializes the global client list mutex.
 *        This function is called once to ensure thread-safe access to the client list.
 */
static void client_list_init_once(void) {
  if (uv_mutex_init(&g_client_list_lock) == 0)
    g_client_list_initialized = 1;
}

/**
 * @brief Registers an asynchronous client instance in the global client list.
 *
 * @param client A pointer to the `async_client_t` instance to register.
 */
static void client_list_register(async_client_t *client) {
  uv_once(&g_client_list_once, client_list_init_once);
  if (!g_client_list_initialized)
    return;
  uv_mutex_lock(&g_client_list_lock);
  client->global_next = g_client_list_head;
  g_client_list_head = client;
  uv_mutex_unlock(&g_client_list_lock);
}

/**
 * @brief Unregisters an asynchronous client instance from the global client list.
 *
 * @param client A pointer to the `async_client_t` instance to unregister.
 */
static void client_list_unregister(async_client_t *client) {
  if (!g_client_list_initialized)
    return;
  uv_mutex_lock(&g_client_list_lock);
  async_client_t **cursor = &g_client_list_head;
  while (*cursor) {
    if (*cursor == client) {
      *cursor = client->global_next;
      break;
    }
    cursor = &(*cursor)->global_next;
  }
  client->global_next = NULL;
  uv_mutex_unlock(&g_client_list_lock);
}

/**
 * @brief Retrieves an `async_client_t` instance associated with a `turbo_pipe_client_t`.
 *
 * @param pipe_client A pointer to the `turbo_pipe_client_t` instance.
 * @return A pointer to the corresponding `async_client_t` instance, or NULL if not found.
 */
static async_client_t *client_from_pipe(turbo_pipe_client_t *pipe_client) {
  if (!g_client_list_initialized)
    return NULL;
  uv_mutex_lock(&g_client_list_lock);
  async_client_t *cursor = g_client_list_head;
  while (cursor) {
    if (cursor->transport == ASYNC_CLIENT_TRANSPORT_PIPE &&
        cursor->proto.pipe.client == pipe_client) {
      uv_mutex_unlock(&g_client_list_lock);
      return cursor;
    }
    cursor = cursor->global_next;
  }
  uv_mutex_unlock(&g_client_list_lock);
  return NULL;
}

/**
 * @brief Retrieves an `async_client_t` instance associated with a `turbo_tls_client_t`.
 *
 * @param tls_client A pointer to the `turbo_tls_client_t` instance.
 * @return A pointer to the corresponding `async_client_t` instance, or NULL if not found.
 */
static async_client_t *client_from_tls_impl(turbo_tls_client_t *tls_client) {
  if (!tls_client)
    return NULL;
  return (async_client_t *)tls_client->user_data;
}

/**
 * @brief Creates a new asynchronous client command.
 *
 * @param type The type of the command to create.
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create(async_client_command_type_t type) {
  async_client_command_t *cmd = (async_client_command_t *)calloc(1, sizeof(*cmd));
  if (!cmd)
    return NULL;
  cmd->type = type;
  return cmd;
}

/**
 * @brief Creates a new connect command.
 *
 * @param host The hostname or IP address to connect to.
 * @param port The port number to connect to.
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create_connect(const char *host, int port) {
  if (!host)
    return NULL;
  async_client_command_t *cmd = command_create(COMMAND_CONNECT);
  if (!cmd)
    return NULL;
  cmd->payload.connect.host = client_common_strdup(host);
  if (!cmd->payload.connect.host) {
    free(cmd);
    return NULL;
  }
  cmd->payload.connect.port = port;
  return cmd;
}

/**
 * @brief Creates a new send command.
 *
 * @param data A pointer to the data buffer to send.
 * @param len The length of the data to send.
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create_send(const char *data, size_t len) {
  async_client_command_t *cmd = command_create(COMMAND_SEND);
  if (!cmd)
    return NULL;
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

/**
 * @brief Creates a new sendv command for scatter-gather send.
 *
 * TRUE ZERO-COPY: Only allocates command + iov array (references user buffers).
 * User buffers are wrapped with arena when command is processed (no memcpy).
 *
 * WARNING: User must ensure buffers remain valid until command is processed!
 * For stack buffers, use the old copying version or ensure they outlive the call.
 *
 * @param iov Array of iovec structures describing the buffers.
 * @param iovcnt Number of elements in the iov array.
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create_sendv(const async_client_iovec_t *iov,
                                                    size_t iovcnt) {
  if (!iov || iovcnt == 0)
    return NULL;

  /* Allocate command + iov array only (NO buffer data copy!) */
  size_t cmd_size = sizeof(async_client_command_t);
  size_t iov_size = iovcnt * sizeof(async_client_iovec_t);
  size_t total_size = cmd_size + iov_size;

  void *mem = malloc(total_size);
  if (!mem)
    return NULL;

  /* Layout memory */
  async_client_command_t *cmd = (async_client_command_t *)mem;
  async_client_iovec_t *iov_array = (async_client_iovec_t *)((char *)mem + cmd_size);

  /* Initialize command */
  memset(cmd, 0, sizeof(async_client_command_t));
  cmd->type = COMMAND_SENDV;
  cmd->payload.sendv.iov = iov_array;
  cmd->payload.sendv.iovcnt = iovcnt;

  /* Copy iov array (just pointers, NOT data!) - ZERO COPY! */
  for (size_t i = 0; i < iovcnt; i++) {
    iov_array[i].data = iov[i].data; /* Reference user buffer */
    iov_array[i].len = iov[i].len;
  }

  return cmd;
}

/**
 * @brief Creates a new sendv_slices command for zero-copy sending with arena slices.
 *
 * This creates a command that holds REFERENCES to arena slices with refcounting.
 * Increases refcount on each slice to ensure buffer remains valid until send completes.
 *
 * @param slices Array of turbo_arena_slice_t with arena-managed buffers.
 * @param slice_count Number of slices in the array.
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create_sendv_slices(const turbo_arena_slice_t *slices,
                                                           size_t slice_count) {
  if (!slices || slice_count == 0)
    return NULL;

  async_client_command_t *cmd = command_create(COMMAND_SENDV_SLICES);
  if (!cmd)
    return NULL;

  /* Allocate slice array (command owns this array) */
  cmd->payload.sendv_slices.slices =
      (turbo_arena_slice_t *)malloc(slice_count * sizeof(turbo_arena_slice_t));
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

  cmd->payload.sendv_slices.slice_count = slice_count;
  return cmd;
}

/**
 * @brief Creates a new close command.
 *
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create_close(void) { return command_create(COMMAND_CLOSE); }

/**
 * @brief Creates a new stop command.
 *
 * @return A pointer to the newly created `async_client_command_t` instance, or NULL on failure.
 */
static async_client_command_t *command_create_stop(void) { return command_create(COMMAND_STOP); }

/**
 * @brief Frees the resources associated with an asynchronous client command.
 *
 * @param cmd A pointer to the `async_client_command_t` instance to free.
 */
static void free_command(async_client_command_t *cmd) {
  if (!cmd)
    return;
  if (cmd->type == COMMAND_CONNECT) {
    free(cmd->payload.connect.host);
  } else if (cmd->type == COMMAND_SEND) {
    free(cmd->payload.send.data);
  } else if (cmd->type == COMMAND_SENDV) {
    /* ZERO-COPY: Command only contains [cmd][iov_array]
     * User buffers are NOT owned by command, just referenced
     * Single free for command + iov array */
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
  }
  free(cmd);
}

/**
 * @brief Submits a command to the client's command queue for asynchronous processing.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param cmd A pointer to the `async_client_command_t` to submit. The command will be freed after
 * processing.
 * @param mark_shutdown If true, marks the client for shutdown, preventing further commands.
 * @return `ASYNC_CLIENT_STATUS_OK` on success, or an error code otherwise.
 */
static async_client_status_t submit_command(async_client_t *client, async_client_command_t *cmd,
                                            int mark_shutdown) {
  if (!client || !cmd)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  async_client_status_t status = ASYNC_CLIENT_STATUS_OK;

  uv_mutex_lock(&client->mutex);
  if (!client->loop_running) {
    status = ASYNC_CLIENT_STATUS_NOT_READY;
  } else if (client->shutting_down) {
    status = ASYNC_CLIENT_STATUS_SHUTTING_DOWN;
  } else {
    if (mark_shutdown)
      client->shutting_down = 1;
    cmd->next = NULL;
    if (client->command_tail)
      client->command_tail->next = cmd;
    else
      client->command_head = cmd;
    client->command_tail = cmd;
  }
  uv_mutex_unlock(&client->mutex);

  if (status == ASYNC_CLIENT_STATUS_OK)
    uv_async_send(&client->command_async);

  return status;
}

/**
 * @brief Callback function for the asynchronous command handle.
 *        Processes commands from the client's command queue in the event loop thread.
 *
 * @param handle A pointer to the `uv_async_t` handle.
 */
static void command_async_cb(uv_async_t *handle) {
  async_client_t *client = (async_client_t *)handle->data;

  for (;;) {
    async_client_command_t *cmd = NULL;

    uv_mutex_lock(&client->mutex);
    if (client->command_head) {
      cmd = client->command_head;
      client->command_head = cmd->next;
      if (!client->command_head)
        client->command_tail = NULL;
    }
    int shutting_down = client->shutting_down;
    uv_mutex_unlock(&client->mutex);

    if (!cmd) {
      if (shutting_down)
        transport_close(client);
      break;
    }

    switch (cmd->type) {
    case COMMAND_CONNECT:
      handle_connect_command(client, cmd);
      break;
    case COMMAND_SEND:
      handle_send_command(client, cmd);
      break;
    case COMMAND_SENDV:
      handle_sendv_command(client, cmd);
      break;
    case COMMAND_SENDV_SLICES:
      handle_sendv_slices_command(client, cmd);
      break;
    case COMMAND_CLOSE:
      handle_close_command(client);
      break;
    case COMMAND_STOP:
      handle_stop_command(client);
      break;
    }

    free_command(cmd);
  }
}

/**
 * @brief The main function for the client's event loop thread.
 *        Initializes the libuv loop and processes commands.
 *
 * @param arg A pointer to the `async_client_t` instance.
 */
static void loop_thread_main(void *arg) {
  async_client_t *client = (async_client_t *)arg;
  int rc = uv_loop_init(&client->loop);
  int async_initialized = 0;

  if (rc == 0) {
    rc = uv_async_init(&client->loop, &client->command_async, command_async_cb);
    if (rc == 0) {
      async_initialized = 1;
      client->command_async.data = client;
    }
  }

  if (rc == 0) {
    async_client_status_t setup_status = transport_setup(client);
    if (setup_status != ASYNC_CLIENT_STATUS_OK)
      rc = UV_EINVAL;
  }

  uv_mutex_lock(&client->mutex);
  client->init_error = rc;
  client->loop_ready = 1;
  client->loop_running = (rc == 0);
  uv_cond_broadcast(&client->cond);
  uv_mutex_unlock(&client->mutex);

  if (rc != 0) {
    if (async_initialized && !uv_is_closing((uv_handle_t *)&client->command_async)) {
      uv_close((uv_handle_t *)&client->command_async, NULL);
      uv_run(&client->loop, UV_RUN_DEFAULT);
    }
    uv_loop_close(&client->loop);
    return;
  }

  uv_run(&client->loop, UV_RUN_DEFAULT);

  uv_mutex_lock(&client->mutex);
  client->loop_running = 0;
  uv_cond_broadcast(&client->cond);
  uv_mutex_unlock(&client->mutex);

  transport_close(client);
  uv_run(&client->loop, UV_RUN_DEFAULT);
  uv_loop_close(&client->loop);
}

/* ========================================================================
 * Vtable implementations for each transport
 * ======================================================================== */

/* Forward declarations for vtable _impl functions */
static async_client_status_t tcp_setup_impl(async_client_t *client);
static void tcp_close_impl(async_client_t *client);
static int tcp_connect_impl(async_client_t *client, const char *host, int port);
static int tcp_send_impl(async_client_t *client, char *data, size_t len);
static int tcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);
static async_client_state_t tcp_get_state_impl(const async_client_t *client);

static async_client_status_t udp_setup_impl(async_client_t *client);
static void udp_close_impl(async_client_t *client);
static int udp_connect_impl(async_client_t *client, const char *host, int port);
static int udp_send_impl(async_client_t *client, char *data, size_t len);
static int udp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);
static async_client_state_t udp_get_state_impl(const async_client_t *client);
static async_client_status_t udp_set_multicast_ttl_impl(async_client_t *client, int ttl);
static async_client_status_t udp_set_multicast_loop_impl(async_client_t *client, int on);

static async_client_status_t kcp_setup_impl(async_client_t *client);
static void kcp_close_impl(async_client_t *client);
static int kcp_connect_impl(async_client_t *client, const char *host, int port);
static int kcp_send_impl(async_client_t *client, char *data, size_t len);
static int kcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);
static async_client_state_t kcp_get_state_impl(const async_client_t *client);

static async_client_status_t tls_setup_impl(async_client_t *client);
static void tls_close_impl(async_client_t *client);
static int tls_connect_impl(async_client_t *client, const char *host, int port);
static int tls_send_impl(async_client_t *client, char *data, size_t len);
static int tls_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);
static async_client_state_t tls_get_state_impl(const async_client_t *client);

static async_client_status_t pipe_setup_impl(async_client_t *client);
static void pipe_close_impl(async_client_t *client);
static int pipe_connect_impl(async_client_t *client, const char *host, int port);
static int pipe_send_impl(async_client_t *client, char *data, size_t len);
static int pipe_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);
static async_client_state_t pipe_get_state_impl(const async_client_t *client);

static async_client_status_t ws_setup_impl(async_client_t *client);
static void ws_close_impl(async_client_t *client);
static int ws_connect_impl(async_client_t *client, const char *host, int port);
static int ws_send_impl(async_client_t *client, char *data, size_t len);
static int ws_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt);
static async_client_state_t ws_get_state_impl(const async_client_t *client);

/* Static vtable instances */
static const async_client_transport_ops_t tcp_ops = {.setup = tcp_setup_impl,
                                                     .close = tcp_close_impl,
                                                     .connect = tcp_connect_impl,
                                                     .send = tcp_send_impl,
                                                     .sendv = tcp_sendv_impl,
                                                     .get_state = tcp_get_state_impl,
                                                     .set_multicast_ttl = NULL,  /* Not supported */
                                                     .set_multicast_loop = NULL, /* Not supported */
                                                     .name = "TCP"};

static const async_client_transport_ops_t udp_ops = {
    .setup = udp_setup_impl,
    .close = udp_close_impl,
    .connect = udp_connect_impl,
    .send = udp_send_impl,
    .sendv = udp_sendv_impl,
    .get_state = udp_get_state_impl,
    .set_multicast_ttl = udp_set_multicast_ttl_impl,   /* UDP-specific */
    .set_multicast_loop = udp_set_multicast_loop_impl, /* UDP-specific */
    .name = "UDP"};

static const async_client_transport_ops_t kcp_ops = {.setup = kcp_setup_impl,
                                                     .close = kcp_close_impl,
                                                     .connect = kcp_connect_impl,
                                                     .send = kcp_send_impl,
                                                     .sendv = kcp_sendv_impl,
                                                     .get_state = kcp_get_state_impl,
                                                     .set_multicast_ttl = NULL,  /* Not supported */
                                                     .set_multicast_loop = NULL, /* Not supported */
                                                     .name = "KCP"};

static const async_client_transport_ops_t tls_ops = {.setup = tls_setup_impl,
                                                     .close = tls_close_impl,
                                                     .connect = tls_connect_impl,
                                                     .send = tls_send_impl,
                                                     .sendv = tls_sendv_impl,
                                                     .get_state = tls_get_state_impl,
                                                     .set_multicast_ttl = NULL,  /* Not supported */
                                                     .set_multicast_loop = NULL, /* Not supported */
                                                     .name = "TLS"};

static const async_client_transport_ops_t pipe_ops = {.setup = pipe_setup_impl,
                                                      .close = pipe_close_impl,
                                                      .connect = pipe_connect_impl,
                                                      .send = pipe_send_impl,
                                                      .sendv = pipe_sendv_impl,
                                                      .get_state = pipe_get_state_impl,
                                                      .set_multicast_ttl = NULL, /* Not supported */
                                                      .set_multicast_loop =
                                                          NULL, /* Not supported */
                                                      .name = "PIPE"};

static const async_client_transport_ops_t ws_ops = {.setup = ws_setup_impl,
                                                    .close = ws_close_impl,
                                                    .connect = ws_connect_impl,
                                                    .send = ws_send_impl,
                                                    .sendv = ws_sendv_impl,
                                                    .get_state = ws_get_state_impl,
                                                    .set_multicast_ttl = NULL,
                                                    .set_multicast_loop = NULL,
                                                    .name = "WebSocket"};

/* ========================================================================
 * TCP vtable implementations
 * ======================================================================== */

static async_client_status_t tcp_setup_impl(async_client_t *client) {
  int rc = uv_tcp_init(&client->loop, &client->proto.tcp.handle);
  if (rc != 0)
    return ASYNC_CLIENT_STATUS_INTERNAL_ERROR;
  client->proto.tcp.handle.data = client;
  client->proto.tcp.connect_req.data = client;
  client->proto.tcp.active = 0;
  return ASYNC_CLIENT_STATUS_OK;
}

static void tcp_close_impl(async_client_t *client) {
  if (client->proto.tcp.active)
    uv_read_stop((uv_stream_t *)&client->proto.tcp.handle);
  client->proto.tcp.active = 0;
  if (!uv_is_closing((uv_handle_t *)&client->proto.tcp.handle))
    uv_close((uv_handle_t *)&client->proto.tcp.handle, tcp_close_cb);
  else
    emit_closed(client);
}

static int tcp_connect_impl(async_client_t *client, const char *host, int port) {
  return client_tcp_connect_impl(client, host, port);
}

static int tcp_send_impl(async_client_t *client, char *data, size_t len) {
  async_client_tcp_send_req_t *send_req =
      (async_client_tcp_send_req_t *)calloc(1, sizeof(*send_req));
  if (!send_req) {
    free(data);
    return -ASYNC_CLIENT_STATUS_ALLOC_FAILED;
  }
  send_req->buffer = data;
  send_req->length = len;
  send_req->is_sendv = 0;
  send_req->req.data = send_req;

  uv_buf_t buf = uv_buf_init(send_req->buffer, (unsigned int)send_req->length);
  int rc =
      uv_write(&send_req->req, (uv_stream_t *)&client->proto.tcp.handle, &buf, 1, tcp_write_cb);
  if (rc != 0) {
    free(send_req->buffer);
    free(send_req);
    return rc;
  }
  return 0;
}

static int tcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt) {
  return client_tcp_sendv_impl(client, iov, iovcnt);
}

static async_client_state_t tcp_get_state_impl(const async_client_t *client) {
  return client->proto.tcp.active ? ASYNC_CLIENT_STATE_CONNECTED : ASYNC_CLIENT_STATE_DISCONNECTED;
}

/* ========================================================================
 * UDP vtable implementations
 * ======================================================================== */

static async_client_status_t udp_setup_impl(async_client_t *client) {
  int rc = uv_udp_init(&client->loop, &client->proto.udp.handle);
  if (rc != 0)
    return ASYNC_CLIENT_STATUS_INTERNAL_ERROR;
  client->proto.udp.handle.data = client;
  client->proto.udp.connected = 0;
  client->proto.udp.recv_active = 0;
  client->proto.udp.remote_addr_len = 0;
  return ASYNC_CLIENT_STATUS_OK;
}

static void udp_close_impl(async_client_t *client) {
  if (client->proto.udp.recv_active)
    uv_udp_recv_stop(&client->proto.udp.handle);
  client->proto.udp.recv_active = 0;
  client->proto.udp.connected = 0;
  client->proto.udp.remote_addr_len = 0;
  if (!uv_is_closing((uv_handle_t *)&client->proto.udp.handle))
    uv_close((uv_handle_t *)&client->proto.udp.handle, udp_close_cb);
  else
    emit_closed(client);
}

static int udp_connect_impl(async_client_t *client, const char *host, int port) {
  return client_udp_connect_impl(client, host, port);
}

static int udp_send_impl(async_client_t *client, char *data, size_t len) {
  if (!client->proto.udp.connected && client->proto.udp.remote_addr_len == 0) {
    free(data);
    return -ASYNC_CLIENT_STATUS_NOT_READY;
  }
  async_client_udp_send_req_t *send_req =
      (async_client_udp_send_req_t *)calloc(1, sizeof(*send_req));
  if (!send_req) {
    free(data);
    return -ASYNC_CLIENT_STATUS_ALLOC_FAILED;
  }
  send_req->buffer = data;
  send_req->length = len;
  send_req->is_sendv = 0;
  send_req->req.data = send_req;
  uv_buf_t buf = uv_buf_init(send_req->buffer, (unsigned int)send_req->length);
  const struct sockaddr *addr =
      client->proto.udp.connected ? NULL : (const struct sockaddr *)&client->proto.udp.remote_addr;
  int rc = uv_udp_send(&send_req->req, &client->proto.udp.handle, &buf, 1, addr, udp_send_cb);
  if (rc != 0) {
    free(send_req->buffer);
    free(send_req);
    return rc;
  }
  return 0;
}

static int udp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt) {
  return client_udp_sendv_impl(client, iov, iovcnt);
}

static async_client_state_t udp_get_state_impl(const async_client_t *client) {
  return client->proto.udp.connected ? ASYNC_CLIENT_STATE_CONNECTED
                                     : ASYNC_CLIENT_STATE_DISCONNECTED;
}

/* UDP-specific: multicast TTL */
static async_client_status_t udp_set_multicast_ttl_impl(async_client_t *client, int ttl) {
  if (ttl < 1 || ttl > 255)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  int rc = uv_udp_set_multicast_ttl(&client->proto.udp.handle, ttl);
  if (rc != 0)
    return ASYNC_CLIENT_STATUS_IO_ERROR;

  return ASYNC_CLIENT_STATUS_OK;
}

/* UDP-specific: multicast loopback */
static async_client_status_t udp_set_multicast_loop_impl(async_client_t *client, int on) {
  int rc = uv_udp_set_multicast_loop(&client->proto.udp.handle, on);
  if (rc != 0)
    return ASYNC_CLIENT_STATUS_IO_ERROR;

  return ASYNC_CLIENT_STATUS_OK;
}

/* ========================================================================
 * KCP vtable implementations
 * ======================================================================== */

static async_client_status_t kcp_setup_impl(async_client_t *client) {
  int rc = turbo_kcp_client_init(&client->proto.kcp.client, &client->loop);
  if (rc != 0)
    return ASYNC_CLIENT_STATUS_INTERNAL_ERROR;
  client->proto.kcp.initialized = 1;
  return ASYNC_CLIENT_STATUS_OK;
}

static void kcp_close_impl(async_client_t *client) {
  if (client->proto.kcp.initialized) {
    turbo_kcp_client_close(&client->proto.kcp.client);
    client->proto.kcp.initialized = 0;
  }
  emit_closed(client);
}

static int kcp_connect_impl(async_client_t *client, const char *host, int port) {
  return client_kcp_connect_impl(client, host, port);
}

static int kcp_send_impl(async_client_t *client, char *data, size_t len) {
  int rc = turbo_kcp_client_send(&client->proto.kcp.client, data, len);
  free(data);
  if (rc == 0)
    start_operation_timer(client);
  return rc;
}

static int kcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt) {
  int rc = client_kcp_sendv_impl(client, iov, iovcnt);
  if (rc == 0)
    start_operation_timer(client);
  return rc;
}

static async_client_state_t kcp_get_state_impl(const async_client_t *client) {
  return client->proto.kcp.initialized ? ASYNC_CLIENT_STATE_CONNECTED
                                       : ASYNC_CLIENT_STATE_DISCONNECTED;
}

/* ========================================================================
 * TLS vtable implementations
 * ======================================================================== */

static async_client_status_t tls_setup_impl(async_client_t *client) {
  client->proto.tls.client = NULL;
  client->proto.tls.context_initialized = 0;
  client->proto.tls.client_created = 0;
  client->proto.tls.handshake_pending = 0;
  return ASYNC_CLIENT_STATUS_OK;
}

static void tls_close_impl(async_client_t *client) {
  if (client->proto.tls.client) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
  }
  if (client->proto.tls.context_initialized) {
    turbo_tls_context_destroy(&client->proto.tls.context);
    client->proto.tls.context_initialized = 0;
  }
  emit_closed(client);
}

static int tls_connect_impl(async_client_t *client, const char *host, int port) {
  return client_tls_connect_impl(client, host, port);
}

static int tls_send_impl(async_client_t *client, char *data, size_t len) {
  int rc = turbo_tls_send(client->proto.tls.client, data, len);
  free(data);
  if (rc == 0)
    start_operation_timer(client);
  return rc;
}

static int tls_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt) {
  int rc = client_tls_sendv_impl(client, iov, iovcnt);
  if (rc == 0)
    start_operation_timer(client);
  return rc;
}

static async_client_state_t tls_get_state_impl(const async_client_t *client) {
  return (client->proto.tls.client && !client->proto.tls.handshake_pending)
             ? ASYNC_CLIENT_STATE_CONNECTED
             : ASYNC_CLIENT_STATE_DISCONNECTED;
}

/* ========================================================================
 * PIPE vtable implementations
 * ======================================================================== */

static async_client_status_t pipe_setup_impl(async_client_t *client) {
  client->proto.pipe.client = NULL;
  return ASYNC_CLIENT_STATUS_OK;
}

static void pipe_close_impl(async_client_t *client) {
  if (client->proto.pipe.client) {
    turbo_pipe_client_close(client->proto.pipe.client);
    client->proto.pipe.client = NULL;
  }
  emit_closed(client);
}

static int pipe_connect_impl(async_client_t *client, const char *host, int port) {
  (void)port; /* Pipe doesn't use port */
  return client_pipe_connect_impl(client, host);
}

static int pipe_send_impl(async_client_t *client, char *data, size_t len) {
  int rc = turbo_pipe_send(client->proto.pipe.client, data, len);
  free(data);
  if (rc == 0)
    start_operation_timer(client);
  return rc;
}

static int pipe_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt) {
  int rc = client_pipe_sendv_impl(client, iov, iovcnt);
  if (rc == 0)
    start_operation_timer(client);
  return rc;
}

static async_client_state_t pipe_get_state_impl(const async_client_t *client) {
  return client->proto.pipe.client ? ASYNC_CLIENT_STATE_CONNECTED : ASYNC_CLIENT_STATE_DISCONNECTED;
}

/**
 * @brief Sets up the transport-specific handles and states for the client.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @return `ASYNC_CLIENT_STATUS_OK` on success, or an error code otherwise.
 */
static async_client_status_t transport_setup(async_client_t *client) {
  /* Initialize timeout timers (common for all transports) */
  int rc = uv_timer_init(&client->loop, &client->connect_timer);
  if (rc != 0)
    return ASYNC_CLIENT_STATUS_INTERNAL_ERROR;
  client->connect_timer.data = client;
  client->connect_timer_active = 0;

  rc = uv_timer_init(&client->loop, &client->operation_timer);
  if (rc != 0) {
    uv_close((uv_handle_t *)&client->connect_timer, NULL);
    return ASYNC_CLIENT_STATUS_INTERNAL_ERROR;
  }
  client->operation_timer.data = client;
  client->operation_timer_active = 0;

  /* Dispatch via vtable - NO SWITCH! */
  return client->ops->setup(client);
}

/**
 * @brief Closes the transport-specific handles and cleans up resources.
 *
 * @param client A pointer to the `async_client_t` instance.
 */
static void transport_close(async_client_t *client) {
  /* Stop and close timeout timers (common for all transports) */
  if (client->connect_timer_active) {
    uv_timer_stop(&client->connect_timer);
    client->connect_timer_active = 0;
  }
  if (client->operation_timer_active) {
    uv_timer_stop(&client->operation_timer);
    client->operation_timer_active = 0;
  }
  if (!uv_is_closing((uv_handle_t *)&client->connect_timer))
    uv_close((uv_handle_t *)&client->connect_timer, NULL);
  if (!uv_is_closing((uv_handle_t *)&client->operation_timer))
    uv_close((uv_handle_t *)&client->operation_timer, NULL);

  /* Dispatch via vtable - NO SWITCH! */
  client->ops->close(client);
}

/* Phase 6b: Helper functions for handle_connect_command */
static void client_start_connect_timeout(async_client_t *client) {
  uv_mutex_lock(&client->mutex);
  client->stats.connection_attempts++;
  int timeout_ms = client->connect_timeout_ms;
  uv_mutex_unlock(&client->mutex);

  if (timeout_ms > 0) {
    int rc = uv_timer_start(&client->connect_timer, connect_timeout_cb, timeout_ms, 0);
    if (rc == 0)
      client->connect_timer_active = 1;
  }
}

static int client_tcp_connect_impl(async_client_t *client, const char *host, int port) {
  struct sockaddr_storage addr;
  int addr_len = 0;
  int rc = turbo_dns_resolve(&client->loop, host, port, &addr, &addr_len);
  if (rc != 0)
    return rc;
  rc = uv_tcp_connect(&client->proto.tcp.connect_req, &client->proto.tcp.handle,
                      (const struct sockaddr *)&addr, tcp_connect_cb);
  if (rc != 0)
    transport_close(client);
  return rc;
}

static int client_udp_connect_impl(async_client_t *client, const char *host, int port) {
  struct sockaddr_storage addr;
  int addr_len = 0;
  int rc = turbo_dns_resolve(&client->loop, host, port, &addr, &addr_len);
  if (rc != 0)
    return rc;
  rc = uv_udp_connect(&client->proto.udp.handle, (const struct sockaddr *)&addr);
  if (rc != 0)
    return rc;
  memcpy(&client->proto.udp.remote_addr, &addr, sizeof(addr));
  client->proto.udp.remote_addr_len = addr_len;
  client->proto.udp.connected = 1;

  /* Check if this is a multicast address and set TTL */
  struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
  if (addr_in->sin_family == AF_INET) {
    unsigned long addr_host = ntohl(addr_in->sin_addr.s_addr);
    unsigned char first_octet = (addr_host >> 24) & 0xFF;
    if (first_octet >= 224 && first_octet <= 239)
      uv_udp_set_multicast_ttl(&client->proto.udp.handle, 32);
  }

  if (!client->proto.udp.recv_active) {
    rc = uv_udp_recv_start(&client->proto.udp.handle, udp_alloc_cb, udp_read_cb);
    if (rc != 0)
      return rc;
    client->proto.udp.recv_active = 1;
  }
  emit_connected(client);
  return 0;
}

static int client_kcp_connect_impl(async_client_t *client, const char *host, int port) {
  if (!client->proto.kcp.initialized) {
    int rc = turbo_kcp_client_init(&client->proto.kcp.client, &client->loop);
    if (rc != 0)
      return rc;
    client->proto.kcp.initialized = 1;
  }
  return turbo_kcp_client_connect(&client->proto.kcp.client, host, (unsigned short)port,
                                  kcp_connect_cb, kcp_recv_cb);
}

static int client_tls_connect_impl(async_client_t *client, const char *host, int port) {
  if (!client->proto.tls.context_initialized) {
    int rc = turbo_tls_context_init(&client->proto.tls.context, TURBO_TLS_CONTEXT_LIB_INIT);
    if (rc != 0)
      return rc;
    client->proto.tls.context_initialized = 1;
    turbo_tls_context_set_verify_flags(&client->proto.tls.context, TURBO_TLS_VERIFY_NONE);
  }

  if (client->proto.tls.client_created && client->proto.tls.client) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    client->proto.tls.handshake_pending = 0;
  }

  client->proto.tls.client = turbo_tls_client_create(&client->loop, &client->proto.tls.context);
  if (!client->proto.tls.client)
    return -ASYNC_CLIENT_STATUS_ALLOC_FAILED;
  client->proto.tls.client_created = 1;
  client->proto.tls.client->user_data = client;
  client->proto.tls.client->handshake_done_cb = tls_handshake_cb;

  if (host && host[0] != '\0')
    turbo_tls_client_set_hostname(client->proto.tls.client, host, strlen(host));

  /* Resolve hostname to IP address */
  struct sockaddr_storage addr;
  int addr_len = 0;
  int rc = turbo_dns_resolve(&client->loop, host, port, &addr, &addr_len);
  if (rc != 0) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    return rc;
  }

  /* Extract IP address string */
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
    return -ASYNC_CLIENT_STATUS_TRANSPORT_ERROR;
  }

  rc = turbo_tls_client_connect(client->proto.tls.client, ip_str, (unsigned short)port, tls_recv_cb,
                                tls_connect_cb, tls_close_cb);
  if (rc != 0) {
    turbo_tls_client_close(client->proto.tls.client);
    client->proto.tls.client = NULL;
    client->proto.tls.client_created = 0;
    return rc;
  }
  client->proto.tls.handshake_pending = 1;
  return 0;
}

static int client_pipe_connect_impl(async_client_t *client, const char *host) {
  if (!client->proto.pipe.client)
    client->proto.pipe.client = turbo_pipe_client_create(&client->loop);
  if (!client->proto.pipe.client)
    return -ASYNC_CLIENT_STATUS_ALLOC_FAILED;
  return turbo_pipe_client_connect(client->proto.pipe.client, host, pipe_recv_cb, pipe_connect_cb,
                                   pipe_close_cb);
}

static void handle_connect_command(async_client_t *client, async_client_command_t *cmd) {
  const char *host = cmd->payload.connect.host;
  int port = cmd->payload.connect.port;

  client_start_connect_timeout(client);

  /* Dispatch via vtable - NO SWITCH! */
  int rc = client->ops->connect(client, host, port);
  if (rc != 0) {
    if (rc == -ASYNC_CLIENT_STATUS_ALLOC_FAILED)
      emit_error_message(client, rc, "allocation failed");
    else if (rc == -ASYNC_CLIENT_STATUS_TRANSPORT_ERROR)
      emit_error_message(client, rc, "transport error");
    else
      emit_uv_error(client, rc, client->ops->name);
  }
}

/**
 * @brief Handles a send command, sending data through the client's transport.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param cmd A pointer to the `async_client_command_t` containing the data to send.
 */
static void handle_send_command(async_client_t *client, async_client_command_t *cmd) {
  char *data = cmd->payload.send.data;
  size_t len = cmd->payload.send.len;

  if (!data || len == 0) {
    emit_error_message(client, -ASYNC_CLIENT_STATUS_INVALID_PARAM, "empty payload");
    return;
  }

  cmd->payload.send.data = NULL;

  /* Update statistics */
  uv_mutex_lock(&client->mutex);
  client->stats.messages_sent++;
  client->stats.bytes_sent += len;
  uv_mutex_unlock(&client->mutex);

  /* Dispatch via vtable - NO SWITCH! */
  int rc = client->ops->send(client, data, len);
  if (rc != 0) {
    if (rc == -ASYNC_CLIENT_STATUS_ALLOC_FAILED)
      emit_error_message(client, rc, "allocation failed");
    else if (rc == -ASYNC_CLIENT_STATUS_NOT_READY)
      emit_error_message(client, rc, "not connected");
    else
      emit_uv_error(client, rc, client->ops->name);
  }
}

/* Phase 6a: Helper functions for handle_sendv_command */
static size_t client_calculate_iov_total_bytes(const async_client_iovec_t *iov, size_t iovcnt) {
  size_t total = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    total += iov[i].len;
  }
  return total;
}

static void client_update_sendv_stats(async_client_t *client, size_t total_bytes, size_t iovcnt) {
  uv_mutex_lock(&client->mutex);
  client->stats.scatter_gather_sends++;
  client->stats.total_iov_buffers_sent += iovcnt;
  client->stats.messages_sent++;
  client->stats.bytes_sent += total_bytes;
  uv_mutex_unlock(&client->mutex);
}

static int client_tcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                 size_t iovcnt) {
  async_client_tcp_sendv_req_t *send_req =
      (async_client_tcp_sendv_req_t *)calloc(1, sizeof(*send_req));
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
  for (size_t i = 0; i < iovcnt; i++) {
    send_req->bufs[i] = uv_buf_init((char *)iov[i].data, (unsigned int)iov[i].len);
    send_req->buffers[i] = (char *)iov[i].data;
  }
  send_req->req.data = send_req;

  int rc = uv_write(&send_req->req, (uv_stream_t *)&client->proto.tcp.handle, send_req->bufs,
                    (unsigned int)iovcnt, tcp_write_cb);
  if (rc != 0) {
    free(send_req->buffers);
    free(send_req->bufs);
    free(send_req);
  }
  return rc;
}

static int client_udp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
                                 size_t iovcnt) {
  if (!client->proto.udp.connected && client->proto.udp.remote_addr_len == 0)
    return -ASYNC_CLIENT_STATUS_NOT_READY;

  async_client_udp_sendv_req_t *send_req =
      (async_client_udp_sendv_req_t *)calloc(1, sizeof(*send_req));
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
  for (size_t i = 0; i < iovcnt; i++) {
    send_req->bufs[i] = uv_buf_init((char *)iov[i].data, (unsigned int)iov[i].len);
    send_req->buffers[i] = (char *)iov[i].data;
  }
  send_req->req.data = send_req;

  const struct sockaddr *addr =
      client->proto.udp.connected ? NULL : (const struct sockaddr *)&client->proto.udp.remote_addr;
  int rc = uv_udp_send(&send_req->req, &client->proto.udp.handle, send_req->bufs,
                       (unsigned int)iovcnt, addr, udp_send_cb);
  if (rc != 0) {
    free(send_req->buffers);
    free(send_req->bufs);
    free(send_req);
  }
  return rc;
}

static int client_kcp_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
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
      rc = turbo_kcp_client_send_buffer(&client->proto.kcp.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

static int client_tls_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
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
      rc = turbo_tls_send_buffer(client->proto.tls.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

static int client_pipe_sendv_impl(async_client_t *client, const async_client_iovec_t *iov,
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
      rc = turbo_pipe_send_buffer(client->proto.pipe.client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }
  return rc;
}

static void handle_sendv_command(async_client_t *client, async_client_command_t *cmd) {
  async_client_iovec_t *iov = cmd->payload.sendv.iov;
  size_t iovcnt = cmd->payload.sendv.iovcnt;

  if (!iov || iovcnt == 0) {
    emit_error_message(client, -ASYNC_CLIENT_STATUS_INVALID_PARAM, "empty scatter-gather payload");
    return;
  }

  cmd->payload.sendv.iov = NULL;

  size_t total_bytes = client_calculate_iov_total_bytes(iov, iovcnt);
  client_update_sendv_stats(client, total_bytes, iovcnt);

  /* Dispatch via vtable - NO SWITCH! */
  int rc = client->ops->sendv(client, iov, iovcnt);
  if (rc != 0) {
    if (rc == -ASYNC_CLIENT_STATUS_NOT_READY)
      emit_error_message(client, rc, "not connected");
    else
      emit_uv_error(client, rc, client->ops->name);
  }
}

/**
 * @brief Handles a sendv_slices command for true zero-copy send with arena slices.
 *
 * This function:
 * 1. Validates the arena slices
 * 2. Converts slices to IOV format for transport layer
 * 3. Updates statistics
 * 4. Dispatches to transport vtable
 * 5. Ownership of slices is transferred to this handler (freed in free_command)
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param cmd A pointer to the `async_client_command_t` instance containing slices.
 */
static void handle_sendv_slices_command(async_client_t *client, async_client_command_t *cmd) {
  turbo_arena_slice_t *slices = cmd->payload.sendv_slices.slices;
  size_t slice_count = cmd->payload.sendv_slices.slice_count;

  if (!slices || slice_count == 0) {
    emit_error_message(client, -ASYNC_CLIENT_STATUS_INVALID_PARAM, "empty arena slices payload");
    return;
  }

  /* Calculate total bytes for stats */
  size_t total_bytes = 0;
  for (size_t i = 0; i < slice_count; i++) {
    total_bytes += slices[i].length;
  }

  client_update_sendv_stats(client, total_bytes, slice_count);

  /* Convert arena slices to iovec format for transport layer */
  async_client_iovec_t *iov =
      (async_client_iovec_t *)malloc(slice_count * sizeof(async_client_iovec_t));
  if (!iov) {
    emit_error_message(client, -ASYNC_CLIENT_STATUS_ALLOC_FAILED, "failed to allocate iov");
    return;
  }

  for (size_t i = 0; i < slice_count; i++) {
    iov[i].data = slices[i].data;
    iov[i].len = slices[i].length;
  }

  /* Dispatch via vtable - NO SWITCH! */
  int rc = client->ops->sendv(client, iov, slice_count);

  free(iov);

  if (rc != 0) {
    if (rc == -ASYNC_CLIENT_STATUS_NOT_READY)
      emit_error_message(client, rc, "not connected");
    else
      emit_uv_error(client, rc, client->ops->name);
  }

  /* Transfer ownership - slices will be freed in free_command() after this returns */
  cmd->payload.sendv_slices.slices = NULL;
  cmd->payload.sendv_slices.slice_count = 0;
}

/**
 * @brief Handles a close command, initiating the shutdown of the client's transport.
 *
 * @param client A pointer to the `async_client_t` instance.
 */
static void handle_close_command(async_client_t *client) {
  transport_close(client);
  emit_closed(client);
}

/**
 * @brief Handles a stop command, initiating a full shutdown of the client's event loop and
 * transport.
 *
 * @param client A pointer to the `async_client_t` instance.
 */
static void handle_stop_command(async_client_t *client) {
  transport_close(client);
  emit_closed(client);
  if (!uv_is_closing((uv_handle_t *)&client->command_async))
    uv_close((uv_handle_t *)&client->command_async, NULL);
  /* Don't call uv_stop() - let loop exit naturally when all handles close */
}

/**
 * @brief Callback function for TCP connection attempts.
 *
 * @param req A pointer to the `uv_connect_t` request.
 * @param status The status of the connection attempt (0 for success, <0 for error).
 */
static void tcp_connect_cb(uv_connect_t *req, int status) {
  async_client_t *client = (async_client_t *)req->handle->data;
  if (status < 0) {
    uv_mutex_lock(&client->mutex);
    client->stats.connection_failures++;
    uv_mutex_unlock(&client->mutex);
    emit_uv_error(client, status, "tcp connect");
    transport_close(client);
    return;
  }

  int rc = uv_read_start((uv_stream_t *)&client->proto.tcp.handle, tcp_alloc_cb, tcp_read_cb);
  if (rc != 0) {
    emit_uv_error(client, rc, "uv_read_start");
    transport_close(client);
    return;
  }

  client->proto.tcp.active = 1;
  emit_connected(client);
}

/**
 * @brief Callback function for TCP write operations.
 *
 * @param req A pointer to the `uv_write_t` request.
 * @param status The status of the write operation (0 for success, <0 for error).
 */
static void tcp_write_cb(uv_write_t *req, int status) {
  async_client_t *client = (async_client_t *)req->handle->data;
  async_client_tcp_send_req_t *send_req = (async_client_tcp_send_req_t *)req->data;

  if (send_req) {
    if (send_req->is_sendv) {
      /* This is actually a sendv request */
      async_client_tcp_sendv_req_t *sendv_req = (async_client_tcp_sendv_req_t *)send_req;
      /* CRITICAL: buffers[i] point to command's single allocation
       * DO NOT free them individually - they're freed with the command */
      free(sendv_req->buffers); /* Free pointer array only */
      free(sendv_req->bufs);    /* Free uv_buf_t array only */
      free(sendv_req);
    } else {
      /* Regular send request */
      free(send_req->buffer);
      free(send_req);
    }
  }

  if (status < 0) {
    emit_uv_error(client, status, "tcp write");
  } else {
    /* Write succeeded - restart operation timeout timer */
    start_operation_timer(client);
  }
}

/**
 * @brief Callback function for TCP buffer allocation.
 *
 * @param handle A pointer to the `uv_handle_t` (stream handle).
 * @param suggested_size The suggested size for the buffer.
 * @param buf A pointer to the `uv_buf_t` to fill with allocated buffer details.
 */
static void tcp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  async_client_t *client = (async_client_t *)handle->data;
  if (suggested_size == 0)
    suggested_size = 4096;

  /* Use arena for zero-copy allocation */
  /* Layout: [arena_buffer_t*][actual data] */
  size_t total_size = sizeof(turbo_arena_buffer_t *) + suggested_size;
  turbo_arena_buffer_t *arena_buf = turbo_arena_get_buffer(&client->arena, total_size);
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

/**
 * @brief Callback function for TCP read operations.
 *
 * @param stream A pointer to the `uv_stream_t` handle.
 * @param nread The number of bytes read, or a negative error code.
 * @param buf A pointer to the `uv_buf_t` containing the received data.
 */
static void tcp_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  async_client_t *client = (async_client_t *)stream->data;

  if (!buf->base)
    return;

  /* Check if buffer is from arena (has header pointer) */
  turbo_arena_buffer_t **header_ptr =
      (turbo_arena_buffer_t **)(buf->base - sizeof(turbo_arena_buffer_t *));
  turbo_arena_buffer_t *arena_buf = NULL;

  /* Validate if this looks like an arena buffer by checking if header points to valid arena */
  if (header_ptr && *header_ptr && (*header_ptr)->arena == &client->arena) {
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
      emit_uv_error(client, (int)nread, "tcp read");
    if (arena_buf)
      turbo_arena_buffer_unref(arena_buf);
    else
      free(buf->base);
    transport_close(client);
    emit_closed(client);
    return;
  }

  /* Data received - restart operation timeout timer */
  start_operation_timer(client);

  if (arena_buf) {
    /* Zero-copy path: create slice and emit */
    turbo_arena_slice_t slice;
    slice.data = buf->base;
    slice.length = (size_t)nread;
    slice.buffer = arena_buf;
    emit_data_slice(client, &slice, 1);
  } else {
    /* Malloc path: transfer ownership */
    emit_data(client, buf->base, (size_t)nread, 1);
  }
}

/**
 * @brief Callback function for TCP handle close operations.
 *
 * @param handle A pointer to the `uv_handle_t` (TCP handle).
 */
static void tcp_close_cb(uv_handle_t *handle) {
  async_client_t *client = (async_client_t *)handle->data;
  client->proto.tcp.active = 0;
  emit_closed(client);
}
/**
 * @brief Callback function for UDP send operations.
 *
 * @param req A pointer to the `uv_udp_send_t` request.
 * @param status The status of the send operation (0 for success, <0 for error).
 */
static void udp_send_cb(uv_udp_send_t *req, int status) {
  async_client_t *client = (async_client_t *)req->handle->data;
  async_client_udp_send_req_t *send_req = (async_client_udp_send_req_t *)req->data;

  if (send_req) {
    if (send_req->is_sendv) {
      /* This is actually a sendv request */
      async_client_udp_sendv_req_t *sendv_req = (async_client_udp_sendv_req_t *)send_req;
      /* CRITICAL: buffers[i] point to command's single allocation
       * DO NOT free them individually - they're freed with the command */
      free(sendv_req->buffers); /* Free pointer array only */
      free(sendv_req->bufs);    /* Free uv_buf_t array only */
      free(sendv_req);
    } else {
      /* Regular send request */
      free(send_req->buffer);
      free(send_req);
    }
  }

  if (status < 0) {
    emit_uv_error(client, status, "udp send");
  } else {
    /* Send succeeded - restart operation timeout timer */
    start_operation_timer(client);
  }
}

/**
 * @brief Callback function for UDP buffer allocation.
 *
 * @param handle A pointer to the `uv_handle_t` (UDP handle).
 * @param suggested_size The suggested size for the buffer.
 * @param buf A pointer to the `uv_buf_t` to fill with allocated buffer details.
 */
static void udp_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  async_client_t *client = (async_client_t *)handle->data;
  if (suggested_size == 0)
    suggested_size = 4096;

  /* Use arena for zero-copy allocation */
  /* Layout: [arena_buffer_t*][actual data] */
  size_t total_size = sizeof(turbo_arena_buffer_t *) + suggested_size;
  turbo_arena_buffer_t *arena_buf = turbo_arena_get_buffer(&client->arena, total_size);
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

/**
 * @brief Callback function for UDP read operations.
 *
 * @param handle A pointer to the `uv_udp_t` handle.
 * @param nread The number of bytes read, or a negative error code.
 * @param buf A pointer to the `uv_buf_t` containing the received data.
 * @param addr A pointer to the `sockaddr` structure of the remote sender.
 * @param flags Additional flags for the read operation.
 */
static void udp_read_cb(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
  UNUSED(addr);
  UNUSED(flags);
  async_client_t *client = (async_client_t *)handle->data;

  if (!buf->base)
    return;

  /* Check if buffer is from arena (has header pointer) */
  turbo_arena_buffer_t **header_ptr =
      (turbo_arena_buffer_t **)(buf->base - sizeof(turbo_arena_buffer_t *));
  turbo_arena_buffer_t *arena_buf = NULL;

  /* Validate if this looks like an arena buffer by checking if header points to valid arena */
  if (header_ptr && *header_ptr && (*header_ptr)->arena == &client->arena) {
    arena_buf = *header_ptr;
  }

  if (nread <= 0) {
    if (nread < 0)
      emit_uv_error(client, (int)nread, "udp recv");
    if (arena_buf)
      turbo_arena_buffer_unref(arena_buf);
    else
      free(buf->base);
    return;
  }

  /* Data received - restart operation timeout timer */
  start_operation_timer(client);

  if (arena_buf) {
    /* Zero-copy path: create slice and emit */
    turbo_arena_slice_t slice;
    slice.data = buf->base;
    slice.length = (size_t)nread;
    slice.buffer = arena_buf;
    emit_data_slice(client, &slice, 1);
  } else {
    /* Malloc path: transfer ownership */
    emit_data(client, buf->base, (size_t)nread, 1);
  }
}

/**
 * @brief Callback function for UDP handle close operations.
 *
 * @param handle A pointer to the `uv_handle_t` (UDP handle).
 */
static void udp_close_cb(uv_handle_t *handle) {
  async_client_t *client = (async_client_t *)handle->data;
  emit_closed(client);
}

/**
 * @brief Callback function for KCP client connection status.
 *
 * @param kcp_client A pointer to the `turbo_kcp_client_t` instance.
 * @param status The connection status (0 for success, non-zero for error).
 */
static void kcp_connect_cb(turbo_kcp_client_t *kcp_client, int status, void *peer) {
  (void)peer;
  async_client_t *client = CONTAINER_OF(kcp_client, async_client_t, proto.kcp.client);
  if (status == 0)
    emit_connected(client);
  else
    emit_uv_error(client, status, "kcp connect");
}

/**
 * @brief Callback function for KCP client received data.
 *
 * @param server A pointer to the `turbo_kcp_server_t` (unused in client context).
 * @param kcp_client A pointer to the `turbo_kcp_client_t` instance.
 * @param data A pointer to the `turbo_arena_slice_t` containing the received data.
 */
static int kcp_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_kcp_client_t *kcp_client = (turbo_kcp_client_t *)handle;
  async_client_t *client = CONTAINER_OF(kcp_client, async_client_t, proto.kcp.client);
  if (!client || !data || data->length == 0)
    return 0;

  emit_data_slice(client, data, 0);
  return 0;
}

/**
 * @brief Callback function for TLS client connection.
 *
 * @param tls_client A pointer to the `turbo_tls_client_t` instance.
 */
static void tls_connect_cb(turbo_tls_client_t *tls_client, int status, void *peer) {
  (void)peer;
  async_client_t *client = client_from_tls_impl(tls_client);
  if (!client)
    return;

  if (status == 0) {
    client->proto.tls.handshake_pending = 0;
    emit_connected(client);
  } else {
    emit_uv_error(client, status, "tls connect");
  }
}

/**
 * @brief Callback function for TLS client close.
 *
 * @param tls_client A pointer to the `turbo_tls_client_t` instance.
 */
static void tls_close_cb(void *handle) {
  turbo_tls_client_t *tls_client = (turbo_tls_client_t *)handle;
  async_client_t *client = client_from_tls_impl(tls_client);
  if (!client)
    return;
  client->proto.tls.client = NULL;
  client->proto.tls.client_created = 0;
  client->proto.tls.handshake_pending = 0;
  emit_closed(client);
}

/**
 * @brief Callback function for TLS handshake completion.
 *
 * @param tls_client A pointer to the `turbo_tls_client_t` instance.
 * @param status The handshake status (0 for success, non-zero for error).
 */
static void tls_handshake_cb(turbo_tls_client_t *tls_client, int status) {
  async_client_t *client = client_from_tls_impl(tls_client);
  if (!client)
    return;

  client->proto.tls.handshake_pending = 0;
  if (status == 0) {
    emit_connected(client);
  } else {
    char buffer[64];
    static const char FMT_TLS_FAIL[32] = "tls handshake failed (%d)";
    stbsp_snprintf(buffer, (int)sizeof(buffer), FMT_TLS_FAIL, status);
    emit_error_message(client, status, buffer);
    turbo_tls_client_close(tls_client);
  }
}

/**
 * @brief Callback function for TLS client received data.
 *
 * @param tls_client A pointer to the `turbo_tls_client_t` instance.
 * @param data A pointer to the `turbo_arena_slice_t` containing the received data.
 * @return 0 on success, or a non-zero error code.
 */
static int tls_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_tls_client_t *tls_client = (turbo_tls_client_t *)handle;
  async_client_t *client = client_from_tls_impl(tls_client);
  if (!client || !data || data->length == 0)
    return 0;
  emit_data_slice(client, data, 0);
  return 0;
}

/**
 * @brief Callback function for Pipe client received data.
 *
 * @param pipe_client A pointer to the `turbo_pipe_client_t` instance.
 * @param data A pointer to the `turbo_arena_slice_t` containing the received data.
 * @return 0 on success, or a non-zero error code.
 */
static int pipe_recv_cb(void *handle, const turbo_arena_slice_t *data, void *peer) {
  UNUSED(peer);
  turbo_pipe_client_t *pipe_client = (turbo_pipe_client_t *)handle;
  async_client_t *client = client_from_pipe(pipe_client);
  if (!client || !data || data->length == 0) {
    if (data)
      turbo_arena_slice_release((turbo_arena_slice_t *)data);
    return 0;
  }

  emit_data_slice(client, data, 1);
  return 0;
}

/**
 * @brief Callback function for Pipe client connection.
 *
 * @param pipe_client A pointer to the `turbo_pipe_client_t` instance.
 */
static void pipe_connect_cb(turbo_pipe_client_t *pipe_client, int status, void *peer) {
  (void)peer;
  async_client_t *client = client_from_pipe(pipe_client);
  if (!client)
    return;

  if (status == 0) {
    emit_connected(client);
  } else {
    emit_uv_error(client, status, "pipe connect");
  }
}

/**
 * @brief Callback function for Pipe client close.
 *
 * @param pipe_client A pointer to the `turbo_pipe_client_t` instance.
 */
static void pipe_close_cb(void *handle) {
  turbo_pipe_client_t *pipe_client = (turbo_pipe_client_t *)handle;
  async_client_t *client = client_from_pipe(pipe_client);
  if (!client)
    return;
  emit_closed(client);
}

/**
 * @brief Emits an asynchronous client event to the user-defined callback.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param type The type of the event.
 * @param data A pointer to the event data (can be NULL for some event types).
 * @param length The length of the event data.
 * @param status The status code associated with the event.
 * @param message A human-readable message associated with the event.
 * @param slice A pointer to a `turbo_arena_slice_t` for zero-copy data (can be NULL).
 * @param flags Additional flags for the event.
 */
static void emit_event(async_client_t *client, async_client_event_type_t type, const char *data,
                       size_t length, int status, const char *message,
                       const turbo_arena_slice_t *slice, async_client_event_flags_t flags) {
  if (!client->callback)
    return;

  async_client_event_t event;
  memset(&event, 0, sizeof(event));
  event.type = type;
  event.data = data;
  event.length = length;
  event.status = status;
  event.slice = slice;
  event.flags = flags;

  if (message && message[0] != '\0') {
    size_t copy_len = sizeof(client->event_message) - 1;
    strncpy(client->event_message, message, copy_len);
    client->event_message[copy_len] = '\0';
    event.message = client->event_message;
  } else {
    client->event_message[0] = '\0';
    event.message = NULL;
  }

  client->callback(client, &event, client->callback_user_data);
}

/**
 * @brief Emits a connected event.
 *
 * @param client A pointer to the `async_client_t` instance.
 */
static void emit_connected(async_client_t *client) {
  /* Stop connect timeout timer on successful connection */
  if (client->connect_timer_active) {
    uv_timer_stop(&client->connect_timer);
    client->connect_timer_active = 0;
  }

  uv_mutex_lock(&client->mutex);
  client->closed_emitted = 0;
  client->close_requested = 0;
  uv_mutex_unlock(&client->mutex);
  emit_event(client, ASYNC_CLIENT_EVENT_CONNECTED, NULL, 0, 0, NULL, NULL,
             ASYNC_CLIENT_EVENT_FLAG_NONE);
}

/**
 * @brief Emits a closed event.
 *
 * @param client A pointer to the `async_client_t` instance.
 */
static void emit_closed(async_client_t *client) {
  int already_emitted = 0;
  uv_mutex_lock(&client->mutex);
  if (client->closed_emitted)
    already_emitted = 1;
  else
    client->closed_emitted = 1;
  uv_mutex_unlock(&client->mutex);

  if (!already_emitted)
    emit_event(client, ASYNC_CLIENT_EVENT_CLOSED, NULL, 0, 0, NULL, NULL,
               ASYNC_CLIENT_EVENT_FLAG_NONE);
}

/**
 * @brief Emits a data event with a copy-based data buffer.
 *
 * Phase 4: Thin wrapper around emit_data_slice() - eliminates code duplication.
 * Creates a temporary slice with buffer=NULL to signal malloc-based memory.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param data A pointer to the received data buffer.
 * @param length The length of the received data.
 * @param free_after If true, the `data` buffer will be freed after the event is emitted.
 */
static void emit_data(async_client_t *client, char *data, size_t length, int free_after) {
  /* Create temporary slice with buffer=NULL to signal malloc-based allocation */
  turbo_arena_slice_t temp_slice = {
      .data = data,
      .length = length,
      .buffer = NULL /* NULL = malloc, will use free() instead of arena release */
  };

  emit_data_slice(client, &temp_slice, free_after);
}

/**
 * @brief Emits a data event with a zero-copy arena slice.
 *
 * Phase 4: Unified data emission - handles both arena and malloc-based buffers.
 * Checks slice->buffer to determine cleanup strategy:
 *   - buffer == NULL: malloc-based, use free(slice->data)
 *   - buffer != NULL: arena-based, use turbo_arena_slice_release()
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param slice A pointer to the `turbo_arena_slice_t` containing the received data.
 * @param release_after If true, the buffer will be freed/released after event emission.
 */
static void emit_data_slice(async_client_t *client, const turbo_arena_slice_t *slice,
                            int release_after) {
  if (!slice || slice->length == 0 || !client->callback) {
    if (release_after && slice) {
      /* Cleanup based on allocation type */
      if (slice->buffer) {
        turbo_arena_slice_release((turbo_arena_slice_t *)slice);
      } else if (slice->data) {
        free(slice->data);
      }
    }
    return;
  }

  /* Update receive statistics */
  uv_mutex_lock(&client->mutex);
  client->stats.messages_received++;
  client->stats.bytes_received += slice->length;
  uv_mutex_unlock(&client->mutex);

  /* Data received - restart operation timeout timer */
  start_operation_timer(client);

  /* Emit event with appropriate flag based on allocation type */
  async_client_event_flags_t flags =
      slice->buffer ? ASYNC_CLIENT_EVENT_FLAG_ZERO_COPY : ASYNC_CLIENT_EVENT_FLAG_NONE;

  emit_event(client, ASYNC_CLIENT_EVENT_DATA, (const char *)slice->data, slice->length, 0, NULL,
             slice->buffer ? slice : NULL, flags);

  /* Cleanup based on allocation type */
  if (release_after) {
    if (slice->buffer) {
      turbo_arena_slice_release((turbo_arena_slice_t *)slice);
    } else if (slice->data) {
      free(slice->data);
    }
  }
}

/**
 * @brief Emits an error event with a custom message.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param status The status code of the error.
 * @param context A human-readable context message for the error.
 */
static void emit_error_message(async_client_t *client, int status, const char *context) {
  /* Update error statistics */
  uv_mutex_lock(&client->mutex);
  client->stats.send_errors++;
  uv_mutex_unlock(&client->mutex);

  char buffer[ASYNC_CLIENT_ERROR_MESSAGE_MAX];
  if (context && context[0] != '\0') {
    strncpy(buffer, context, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
  } else
    buffer[0] = '\0';
  emit_event(client, ASYNC_CLIENT_EVENT_ERROR, NULL, 0, status, buffer[0] ? buffer : NULL, NULL,
             ASYNC_CLIENT_EVENT_FLAG_NONE);
}

/**
 * @brief Emits an error event with a libuv-specific error message.
 *
 * @param client A pointer to the `async_client_t` instance.
 * @param status The libuv error code.
 * @param context A human-readable context message for the error.
 */
static void emit_uv_error(async_client_t *client, int status, const char *context) {
  /* Update error statistics */
  uv_mutex_lock(&client->mutex);
  if (context && (strstr(context, "send") || strstr(context, "write"))) {
    client->stats.send_errors++;
  } else if (context && (strstr(context, "recv") || strstr(context, "read"))) {
    client->stats.receive_errors++;
  }
  uv_mutex_unlock(&client->mutex);

  const char *uv_msg = uv_strerror(status);
  char buffer[ASYNC_CLIENT_ERROR_MESSAGE_MAX];

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
  emit_event(client, ASYNC_CLIENT_EVENT_ERROR, NULL, 0, status, buffer, NULL,
             ASYNC_CLIENT_EVENT_FLAG_NONE);
}

const char *async_client_status_to_string(async_client_status_t status) {
  switch (status) {
  case ASYNC_CLIENT_STATUS_OK:
    return "ok";
  case ASYNC_CLIENT_STATUS_INVALID_PARAM:
    return "invalid parameter";
  case ASYNC_CLIENT_STATUS_ALLOC_FAILED:
    return "allocation failure";
  case ASYNC_CLIENT_STATUS_NOT_READY:
    return "client not ready";
  case ASYNC_CLIENT_STATUS_SHUTTING_DOWN:
    return "client shutting down";
  case ASYNC_CLIENT_STATUS_IO_ERROR:
    return "I/O error";
  case ASYNC_CLIENT_STATUS_TRANSPORT_ERROR:
    return "transport error";
  case ASYNC_CLIENT_STATUS_INTERNAL_ERROR:
    return "internal error";
  default:
    return "unknown error";
  }
}

const char *async_client_transport_to_string(async_client_transport_t transport) {
  switch (transport) {
  case ASYNC_CLIENT_TRANSPORT_TCP:
    return "tcp";
  case ASYNC_CLIENT_TRANSPORT_UDP:
    return "udp";
  case ASYNC_CLIENT_TRANSPORT_KCP:
    return "kcp";
  case ASYNC_CLIENT_TRANSPORT_TLS:
    return "tls";
  case ASYNC_CLIENT_TRANSPORT_PIPE:
    return "pipe";
  case ASYNC_CLIENT_TRANSPORT_WEBSOCKET:
    return "websocket";
  default:
    return "unknown";
  }
}

async_client_t *async_client_create(async_client_transport_t transport,
                                    async_client_event_cb callback, void *user_data) {
  if (!callback)
    return NULL;

  async_client_t *client = (async_client_t *)calloc(1, sizeof(*client));
  if (!client)
    return NULL;

  client->transport = transport;

  /* Set transport operations vtable based on transport type */
  switch (transport) {
  case ASYNC_CLIENT_TRANSPORT_TCP:
    client->ops = &tcp_ops;
    break;
  case ASYNC_CLIENT_TRANSPORT_UDP:
    client->ops = &udp_ops;
    break;
  case ASYNC_CLIENT_TRANSPORT_KCP:
    client->ops = &kcp_ops;
    break;
  case ASYNC_CLIENT_TRANSPORT_TLS:
    client->ops = &tls_ops;
    break;
  case ASYNC_CLIENT_TRANSPORT_PIPE:
    client->ops = &pipe_ops;
    break;
  case ASYNC_CLIENT_TRANSPORT_WEBSOCKET:
    client->ops = &ws_ops;
    break;
  default:
    free(client);
    return NULL;
  }

  client->callback = callback;
  client->callback_user_data = user_data;

  if (uv_mutex_init(&client->mutex) != 0) {
    free(client);
    return NULL;
  }
  if (uv_cond_init(&client->cond) != 0) {
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  /* Initialize arena for zero-copy allocations */
  if (turbo_arena_init(&client->arena, 64 * 1024) != 0) {
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  if (client_common_config_acquire() == 0) {
    turbo_arena_free(&client->arena);
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }
  client->config_acquired = 1;

  if (dns_resolver_init() != 0) {
    client_common_config_release();
    client->config_acquired = 0;
    turbo_arena_free(&client->arena);
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  client_list_register(client);

  int rc = uv_thread_create(&client->loop_thread, loop_thread_main, client);
  if (rc != 0) {
    client_list_unregister(client);
    client_common_config_release();
    client->config_acquired = 0;
    turbo_arena_free(&client->arena);
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  uv_mutex_lock(&client->mutex);
  while (!client->loop_ready)
    uv_cond_wait(&client->cond, &client->mutex);
  rc = client->init_error;
  uv_mutex_unlock(&client->mutex);

  if (rc != 0) {
    uv_thread_join(&client->loop_thread);
    client_list_unregister(client);
    client_common_config_release();
    client->config_acquired = 0;
    turbo_arena_free(&client->arena);
    uv_cond_destroy(&client->cond);
    uv_mutex_destroy(&client->mutex);
    free(client);
    return NULL;
  }

  return client;
}

void async_client_destroy(async_client_t *client) {
  if (!client)
    return;

  async_client_close(client);

  async_client_command_t *stop_cmd = command_create_stop();
  if (stop_cmd) {
    if (submit_command(client, stop_cmd, 1) != ASYNC_CLIENT_STATUS_OK)
      free_command(stop_cmd);
  }

  uv_thread_join(&client->loop_thread);

  client_list_unregister(client);

  dns_resolver_cleanup();

  if (client->config_acquired) {
    client_common_config_release();
    client->config_acquired = 0;
  }

  turbo_arena_free(&client->arena);
  uv_cond_destroy(&client->cond);
  uv_mutex_destroy(&client->mutex);
  free(client);
}

async_client_status_t async_client_connect(async_client_t *client, const char *host, int port) {
  if (!client || !host)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  async_client_command_t *cmd = command_create_connect(host, port);
  if (!cmd)
    return ASYNC_CLIENT_STATUS_ALLOC_FAILED;

  async_client_status_t status = submit_command(client, cmd, 0);
  if (status != ASYNC_CLIENT_STATUS_OK)
    free_command(cmd);
  return status;
}

async_client_status_t async_client_send(async_client_t *client, const char *data, size_t len) {
  if (!client || (!data && len > 0))
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;
  if (len == 0)
    return ASYNC_CLIENT_STATUS_OK;

  /* Check if connected before accepting send */
  if (!async_client_is_connected(client))
    return ASYNC_CLIENT_STATUS_NOT_READY;

  async_client_command_t *cmd = command_create_send(data, len);
  if (!cmd)
    return ASYNC_CLIENT_STATUS_ALLOC_FAILED;

  async_client_status_t status = submit_command(client, cmd, 0);
  if (status != ASYNC_CLIENT_STATUS_OK)
    free_command(cmd);
  return status;
}

async_client_status_t async_client_sendv(async_client_t *client, const async_client_iovec_t *iov,
                                         size_t iovcnt) {
  if (!client || !iov || iovcnt == 0)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Check if connected before accepting send */
  if (!async_client_is_connected(client))
    return ASYNC_CLIENT_STATUS_NOT_READY;

  /* REFACTORED: Allocate from arena and copy user data (convenience API)
   * This provides safety - user buffers can be freed immediately after call returns.
   * One memcpy per buffer is the trade-off for simplicity. */

  turbo_arena_slice_t *slices = (turbo_arena_slice_t *)malloc(iovcnt * sizeof(turbo_arena_slice_t));
  if (!slices)
    return ASYNC_CLIENT_STATUS_ALLOC_FAILED;

  /* Allocate arena buffers and copy user data */
  size_t allocated = 0;
  for (size_t i = 0; i < iovcnt; i++) {
    if (iov[i].len == 0) {
      /* Skip empty buffers */
      continue;
    }

    /* Get buffer from arena */
    turbo_arena_buffer_t *buf = turbo_arena_get_buffer(&client->arena, iov[i].len);
    if (!buf) {
      /* Cleanup already allocated slices */
      for (size_t j = 0; j < allocated; j++) {
        turbo_arena_buffer_unref(slices[j].buffer);
      }
      free(slices);
      return ASYNC_CLIENT_STATUS_ALLOC_FAILED;
    }

    /* Copy user data into arena buffer */
    memcpy(buf->data, iov[i].data, iov[i].len);
    buf->used = iov[i].len;

    /* Create slice from buffer */
    slices[allocated] = turbo_arena_buffer_slice(buf, 0, iov[i].len);
    allocated++;
  }

  if (allocated == 0) {
    /* All buffers were empty */
    free(slices);
    return ASYNC_CLIENT_STATUS_OK;
  }

  /* Call zero-copy API with arena slices */
  async_client_status_t status = async_client_sendv_slices(client, slices, allocated);

  /* Release our references (command holds its own) */
  for (size_t i = 0; i < allocated; i++) {
    turbo_arena_buffer_unref(slices[i].buffer);
  }
  free(slices);

  return status;
}

async_client_status_t async_client_sendv_slices(async_client_t *client,
                                                const turbo_arena_slice_t *slices,
                                                size_t slice_count) {
  if (!client || !slices || slice_count == 0)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Validate that all slices have buffers (safety check) */
  for (size_t i = 0; i < slice_count; i++) {
    if (!slices[i].buffer || !slices[i].data) {
      return ASYNC_CLIENT_STATUS_INVALID_PARAM;
    }
  }

  /* Check if connected before accepting send */
  if (!async_client_is_connected(client))
    return ASYNC_CLIENT_STATUS_NOT_READY;

  async_client_command_t *cmd = command_create_sendv_slices(slices, slice_count);
  if (!cmd)
    return ASYNC_CLIENT_STATUS_ALLOC_FAILED;

  async_client_status_t status = submit_command(client, cmd, 0);
  if (status != ASYNC_CLIENT_STATUS_OK)
    free_command(cmd);
  return status;
}

void async_client_close(async_client_t *client) {
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  if (client->close_requested || client->shutting_down) {
    uv_mutex_unlock(&client->mutex);
    return;
  }
  client->close_requested = 1;
  uv_mutex_unlock(&client->mutex);

  async_client_command_t *cmd = command_create_close();
  if (!cmd)
    return;
  if (submit_command(client, cmd, 0) != ASYNC_CLIENT_STATUS_OK)
    free_command(cmd);
}

async_client_state_t async_client_get_state(const async_client_t *client) {
  if (!client)
    return ASYNC_CLIENT_STATE_DISCONNECTED;

  /* This is a simplified state check - could be enhanced */
  if (client->shutting_down || client->close_requested)
    return ASYNC_CLIENT_STATE_CLOSING;

  if (!client->loop_running)
    return ASYNC_CLIENT_STATE_DISCONNECTED;

  /* Dispatch via vtable - NO SWITCH! */
  return client->ops->get_state(client);
}

int async_client_is_connected(const async_client_t *client) {
  return async_client_get_state(client) == ASYNC_CLIENT_STATE_CONNECTED;
}

/**
 * @brief Callback for connect timeout.
 *
 * This callback is triggered when a connection attempt exceeds the configured
 * connect timeout. It emits an error event and closes the transport.
 */
static void connect_timeout_cb(uv_timer_t *timer) {
  async_client_t *client = (async_client_t *)timer->data;
  if (!client)
    return;

  client->connect_timer_active = 0;
  emit_error_message(client, -ASYNC_CLIENT_STATUS_IO_ERROR, "connection timeout");

  /* Update connection failure statistics */
  uv_mutex_lock(&client->mutex);
  client->stats.connection_failures++;
  uv_mutex_unlock(&client->mutex);

  transport_close(client);
}

/**
 * @brief Callback for operation timeout.
 *
 * This callback is triggered when an operation (send/receive) exceeds the
 * configured operation timeout. It emits an error event and closes the transport.
 * The operation timeout acts as an idle timeout - it's restarted on each send/receive.
 */
static void operation_timeout_cb(uv_timer_t *timer) {
  async_client_t *client = (async_client_t *)timer->data;
  if (!client)
    return;

  client->operation_timer_active = 0;
  emit_error_message(client, -ASYNC_CLIENT_STATUS_IO_ERROR, "operation timeout");
  transport_close(client);
}

/**
 * @brief Starts or restarts the operation timeout timer.
 *
 * This function is called after send/receive operations to implement an idle timeout.
 * If the timer is already active, it's restarted. If timeout is not configured, does nothing.
 *
 * @param client A pointer to the `async_client_t` instance.
 */
static void start_operation_timer(async_client_t *client) {
  if (!client)
    return;

  /* Get timeout value (thread-safe) */
  uv_mutex_lock(&client->mutex);
  int timeout_ms = client->operation_timeout_ms;
  uv_mutex_unlock(&client->mutex);

  /* Only start timer if timeout is configured */
  if (timeout_ms <= 0) {
    return;
  }

  /* Stop existing timer if active */
  if (client->operation_timer_active) {
    uv_timer_stop(&client->operation_timer);
  }

  /* Start/restart the timer */
  int rc = uv_timer_start(&client->operation_timer, operation_timeout_cb, timeout_ms, 0);
  if (rc == 0) {
    client->operation_timer_active = 1;
  }
}

void async_client_set_connect_timeout(async_client_t *client, int timeout_ms) {
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  client->connect_timeout_ms = timeout_ms;
  uv_mutex_unlock(&client->mutex);
}

void async_client_set_operation_timeout(async_client_t *client, int timeout_ms) {
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  client->operation_timeout_ms = timeout_ms;
  uv_mutex_unlock(&client->mutex);
}

void async_client_get_stats(const async_client_t *client, async_client_stats_t *stats) {
  if (!client || !stats)
    return;

  uv_mutex_lock((uv_mutex_t *)&client->mutex);
  memcpy(stats, &client->stats, sizeof(async_client_stats_t));
  uv_mutex_unlock((uv_mutex_t *)&client->mutex);
}

void async_client_reset_stats(async_client_t *client) {
  if (!client)
    return;

  uv_mutex_lock(&client->mutex);
  memset(&client->stats, 0, sizeof(async_client_stats_t));
  uv_mutex_unlock(&client->mutex);
}

/**
 * @brief Set multicast TTL for UDP client (UDP only).
 */
async_client_status_t async_client_set_multicast_ttl(async_client_t *client, int ttl) {
  if (!client)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Use vtable - no protocol type checking! */
  if (!client->ops->set_multicast_ttl)
    return ASYNC_CLIENT_STATUS_TRANSPORT_ERROR; /* Not supported by this transport */

  return client->ops->set_multicast_ttl(client, ttl);
}

/**
 * @brief Enable/disable multicast loopback for UDP client (UDP only).
 */
async_client_status_t async_client_set_multicast_loop(async_client_t *client, int on) {
  if (!client)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  /* Use vtable - no protocol type checking! */
  if (!client->ops->set_multicast_loop)
    return ASYNC_CLIENT_STATUS_TRANSPORT_ERROR; /* Not supported by this transport */

  return client->ops->set_multicast_loop(client, on);
}

/* ========================================================================
 * WebSocket vtable implementations
 * ======================================================================== */

static int ws_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer);
static void ws_on_connect(void *handle, int status, void *peer);
static void ws_on_close(void *handle);

static async_client_status_t ws_setup_impl(async_client_t *client) {
  client->proto.ws.client = NULL;
  client->proto.ws.config_set = 0;
  client->proto.ws.use_tls = 0;
  memset(&client->proto.ws.config, 0, sizeof(client->proto.ws.config));
  return ASYNC_CLIENT_STATUS_OK;
}

static void ws_close_impl(async_client_t *client) {
  if (client->proto.ws.client) {
    turbo_websocket_client_t *ws = client->proto.ws.client;

    /* Use the standard destroy which handles transport cleanup.
     * The underlying TCP handles will be closed via uv_close.
     * Loop will process close callbacks after transport_close returns. */
    turbo_websocket_client_destroy(ws);
    client->proto.ws.client = NULL;
  }
  emit_closed(client);
}

static int ws_connect_impl(async_client_t *client, const char *host, int port) {
  if (!client->proto.ws.config_set) {
    emit_error_message(client, -1,
                       "WebSocket config not set, call async_client_set_ws_config first");
    return -1;
  }

  turbo_websocket_client_t *ws = turbo_websocket_client_create(
      &client->loop, client->proto.ws.use_tls, &client->proto.ws.config);
  if (!ws) {
    emit_error_message(client, -1, "Failed to create WebSocket client");
    return -1;
  }

  client->proto.ws.client = ws;
  ws->user_data = client;

  turbo_websocket_client_set_callbacks(ws, ws_on_recv, ws_on_connect, ws_on_close);

  int rc = turbo_websocket_client_connect(ws, host, port);
  if (rc != 0) {
    turbo_websocket_client_destroy(ws);
    client->proto.ws.client = NULL;
    emit_error_message(client, rc, "WebSocket connect failed");
    return rc;
  }

  client_start_connect_timeout(client);
  return 0;
}

static int ws_send_impl(async_client_t *client, char *data, size_t len) {
  if (!client->proto.ws.client) {
    free(data);
    return -1;
  }

  int rc = turbo_websocket_client_send(client->proto.ws.client, data, len);
  free(data);

  if (rc == 0) {
    client->stats.bytes_sent += len;
    client->stats.messages_sent++;
    start_operation_timer(client);
  } else {
    client->stats.send_errors++;
  }

  return rc;
}

static int ws_sendv_impl(async_client_t *client, const async_client_iovec_t *iov, size_t iovcnt) {
  if (!client->proto.ws.client)
    return -1;

  size_t total_len = client_calculate_iov_total_bytes(iov, iovcnt);
  int rc = turbo_websocket_client_sendv(client->proto.ws.client, iov, (int)iovcnt);

  if (rc == 0) {
    client_update_sendv_stats(client, total_len, iovcnt);
    start_operation_timer(client);
  } else {
    client->stats.send_errors++;
  }

  return rc;
}

static async_client_state_t ws_get_state_impl(const async_client_t *client) {
  if (!client->proto.ws.client)
    return ASYNC_CLIENT_STATE_DISCONNECTED;

  turbo_websocket_state_t state = turbo_websocket_client_get_state(client->proto.ws.client);
  switch (state) {
  case TURBO_WS_STATE_OPEN:
    return ASYNC_CLIENT_STATE_CONNECTED;
  case TURBO_WS_STATE_CONNECTING:
  case TURBO_WS_STATE_HANDSHAKING:
    return ASYNC_CLIENT_STATE_CONNECTING;
  case TURBO_WS_STATE_CLOSING:
    return ASYNC_CLIENT_STATE_CLOSING;
  case TURBO_WS_STATE_CLOSED:
  default:
    return ASYNC_CLIENT_STATE_DISCONNECTED;
  }
}

static int ws_on_recv(void *handle, const turbo_arena_slice_t *data, void *peer) {
  (void)peer;
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  async_client_t *client = (async_client_t *)ws->user_data;

  if (!client || !data)
    return 0;

  client->stats.bytes_received += data->length;
  client->stats.messages_received++;

  emit_data_slice(client, data, 0);
  return 0;
}

static void ws_on_connect(void *handle, int status, void *peer) {
  (void)peer;
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  async_client_t *client = (async_client_t *)ws->user_data;

  if (!client)
    return;

  if (client->connect_timer_active) {
    uv_timer_stop(&client->connect_timer);
    client->connect_timer_active = 0;
  }

  if (status == 0) {
    client->stats.connection_attempts++;
    emit_connected(client);
  } else {
    client->stats.connection_failures++;
    emit_error_message(client, status, "WebSocket handshake failed");
  }
}

static void ws_on_close(void *handle) {
  turbo_websocket_client_t *ws = (turbo_websocket_client_t *)handle;
  async_client_t *client = (async_client_t *)ws->user_data;

  if (!client)
    return;

  emit_closed(client);
}

async_client_status_t async_client_set_ws_config(async_client_t *client,
                                                 const async_client_ws_config_t *config) {
  if (!client || !config)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  if (client->transport != ASYNC_CLIENT_TRANSPORT_WEBSOCKET)
    return ASYNC_CLIENT_STATUS_INVALID_PARAM;

  uv_mutex_lock(&client->mutex);

  client->proto.ws.config.path = config->path;
  client->proto.ws.config.origin = config->origin;
  client->proto.ws.config.subprotocols = config->subprotocols;
  client->proto.ws.config.subprotocol_count = config->subprotocol_count;
  client->proto.ws.use_tls = config->use_tls;
  client->proto.ws.config_set = 1;

  uv_mutex_unlock(&client->mutex);

  return ASYNC_CLIENT_STATUS_OK;
}
