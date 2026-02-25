/*
 * turbo_kcp.c - KCP reliable UDP transport using ikcp library
 *
 * Uses skywind3000/kcp for ARQ, fast retransmit, and congestion control.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "internal.h"
#include "arena_buffer.h"
#include "stats.h"
#include "turbo_kcp.h"
#include "ikcp.h"

/* Forward declarations for global synchronization */
extern void turbo_kcp_sync_lock(void);
extern void turbo_kcp_sync_unlock(void);

/* Conversation ID allocation */
static uv_once_t g_kcp_conv_once = UV_ONCE_INIT;
static uv_mutex_t g_kcp_conv_lock;
static int g_kcp_conv_lock_initialized = 0;
static uint32_t g_kcp_next_conv_id = 0;

static void turbo_kcp_conv_init_once(void) {
  if (uv_mutex_init(&g_kcp_conv_lock) == 0) {
    g_kcp_conv_lock_initialized = 1;
  }
  g_kcp_next_conv_id = turbo_kcp_config_get_conv_base();
}

static uint32_t turbo_kcp_alloc_conv_id(void) {
  uv_once(&g_kcp_conv_once, turbo_kcp_conv_init_once);
  if (g_kcp_conv_lock_initialized) {
    uv_mutex_lock(&g_kcp_conv_lock);
  }
  uint32_t conv_id = g_kcp_next_conv_id++;
  if (g_kcp_conv_lock_initialized) {
    uv_mutex_unlock(&g_kcp_conv_lock);
  }
  return conv_id;
}

/* KCP context wrapping ikcpcb */
struct turbo_kcp_context_s {
  ikcpcb *kcp;      /* Real KCP control block */
  uint32_t conv_id; /* Conversation ID */
};

/* Send operation for zero-copy */
typedef struct turbo_kcp_send_op_s {
  uv_udp_send_t req;
  turbo_arena_slice_t slice;
  turbo_kcp_server_t *server;
  struct turbo_kcp_send_op_s *next;
} turbo_kcp_send_op_t;

/* Client mapping for server */
typedef struct turbo_kcp_client_mapping_s {
  uint32_t conv_id;
  turbo_kcp_client_t *client;
  struct turbo_kcp_client_mapping_s *next;
} turbo_kcp_client_mapping_t;

/* Simple hash table for client mapping */
#define KCP_CLIENT_MAP_SIZE 256
static turbo_kcp_client_mapping_t *g_client_map[KCP_CLIENT_MAP_SIZE] = {0};

/* Send operation pool */
static turbo_kcp_send_op_t *g_send_op_pool = NULL;
static size_t g_send_op_pool_size = 0;
static const size_t MAX_SEND_OP_POOL_SIZE = 512;

/* KCP Statistics IDs */
static struct {
    turbo_stat_id_t bytes_sent;
    turbo_stat_id_t bytes_received;
    turbo_stat_id_t send_errors;
    turbo_stat_id_t recv_errors;
    turbo_stat_id_t active_connections;
    turbo_stat_id_t connections_closed;
    int initialized;
} s_kcp_stats = {0};

static void init_kcp_stats(void) {
    if (s_kcp_stats.initialized) return;
    
    s_kcp_stats.bytes_sent = turbo_stats_register("kcp.bytes_sent", TURBO_STAT_COUNTER);
    s_kcp_stats.bytes_received = turbo_stats_register("kcp.bytes_received", TURBO_STAT_COUNTER);
    s_kcp_stats.send_errors = turbo_stats_register("kcp.send_errors", TURBO_STAT_COUNTER);
    s_kcp_stats.recv_errors = turbo_stats_register("kcp.recv_errors", TURBO_STAT_COUNTER);
    s_kcp_stats.active_connections = turbo_stats_register("kcp.active_connections", TURBO_STAT_GAUGE);
    s_kcp_stats.connections_closed = turbo_stats_register("kcp.connections_closed", TURBO_STAT_COUNTER);
    
    s_kcp_stats.initialized = 1;
}

/* Connection handshake */
#define KCP_CONN_REQUEST 0x00000000
#define KCP_CONN_SYN "KCP_SYN"
#define KCP_CONN_ACK "KCP_ACK"

/* Hash function for client mapping */
static uint32_t kcp_hash_conv_id(uint32_t conv_id) {
  return conv_id % KCP_CLIENT_MAP_SIZE;
}

/* Find client by conversation ID */
static turbo_kcp_client_t *find_client_by_conv_id(uint32_t conv_id) {
  uint32_t hash = kcp_hash_conv_id(conv_id);
  turbo_kcp_client_mapping_t *mapping = g_client_map[hash];

  while (mapping) {
    if (mapping->conv_id == conv_id) {
      return mapping->client;
    }
    mapping = mapping->next;
  }
  return NULL;
}

/* Helper to safely copy sockaddr to sockaddr_storage */
static void safe_copy_sockaddr(struct sockaddr_storage *dest, const struct sockaddr *src) {
  if (!dest || !src) return;
  memset(dest, 0, sizeof(struct sockaddr_storage));
  if (src->sa_family == AF_INET6) {
    memcpy(dest, src, sizeof(struct sockaddr_in6));
  } else {
    /* Default to IPv4 size for safety if it's AF_INET or other */
    memcpy(dest, src, sizeof(struct sockaddr_in));
  }
}


/* Add client mapping */
static void add_client_mapping(uint32_t conv_id, turbo_kcp_client_t *client) {
  uint32_t hash = kcp_hash_conv_id(conv_id);
  turbo_kcp_client_mapping_t *mapping = malloc(sizeof(turbo_kcp_client_mapping_t));
  if (!mapping) return;

  mapping->conv_id = conv_id;
  mapping->client = client;
  
  /* Insert at head of the bucket list */
  mapping->next = g_client_map[hash];
  g_client_map[hash] = mapping;
  
  /* Update active connections (simplified for now, ideally track with atomic or context) */
  /* turbo_stats_gauge_set_fast(s_kcp_stats.active_connections, ...); */
}

/* Remove client mapping */
static void remove_client_mapping(uint32_t conv_id) {
  uint32_t hash = kcp_hash_conv_id(conv_id);
  turbo_kcp_client_mapping_t **mapping = &g_client_map[hash];

  while (*mapping) {
    if ((*mapping)->conv_id == conv_id) {
      turbo_kcp_client_mapping_t *to_remove = *mapping;
      *mapping = (*mapping)->next;
      free(to_remove);
      turbo_stats_counter_inc_fast(s_kcp_stats.connections_closed);
      return;
    }
    mapping = &(*mapping)->next;
  }
}

/* Get send operation from pool */
static turbo_kcp_send_op_t *get_send_op(turbo_kcp_server_t *server) {
  turbo_kcp_send_op_t *op = NULL;

  turbo_kcp_sync_lock();
  if (g_send_op_pool && g_send_op_pool_size > 0) {
    op = g_send_op_pool;
    g_send_op_pool = op->next;
    g_send_op_pool_size--;
  }
  turbo_kcp_sync_unlock();

  if (!op) {
    op = (turbo_kcp_send_op_t *)malloc(sizeof(turbo_kcp_send_op_t));
    if (op) {
    } else {
      return NULL;
    }
  }

  memset(op, 0, sizeof(*op));
  op->server = server;
  return op;
}

/* Return send operation to pool */
static void return_send_op(turbo_kcp_send_op_t *op) {
  if (!op) return;

  turbo_arena_slice_release(&op->slice);

    turbo_kcp_sync_lock();
    if (g_send_op_pool_size < MAX_SEND_OP_POOL_SIZE) {
        op->next = g_send_op_pool;
        g_send_op_pool = op;
        g_send_op_pool_size++;
        turbo_kcp_sync_unlock();
    } else {
        turbo_kcp_sync_unlock();
        free(op);
    }
}

/* Send completion callback */
static void on_send_complete(uv_udp_send_t *req, int status) {
  turbo_kcp_send_op_t *op =
      (turbo_kcp_send_op_t *)((char *)req - offsetof(turbo_kcp_send_op_t, req));

  if (status == 0) {
    turbo_stats_counter_add_fast(s_kcp_stats.bytes_sent, op->slice.length);
  } else {
    turbo_stats_counter_inc_fast(s_kcp_stats.send_errors);
  }

  return_send_op(op);
}

/* Get current timestamp in milliseconds */
static IUINT32 kcp_get_timestamp(void) {
  return (IUINT32)(uv_hrtime() / 1000000);
}

/* ikcp output callback - sends UDP packets */
static int ikcp_output_callback(const char *buf, int len, ikcpcb *kcp, void *user) {
  turbo_kcp_client_t *client = (turbo_kcp_client_t *)user;
  if (!client || !client->server || !buf || len <= 0) {
    return -1;
  }

  turbo_kcp_send_op_t *op = get_send_op(client->server);
  if (!op) return -1;

  turbo_arena_buffer_t *buffer = turbo_arena_get_buffer(&client->server->arena, len);
  if (!buffer) {
    return_send_op(op);
    return -1;
  }

  memcpy(buffer->data, buf, len);
  turbo_arena_buffer_set_used(buffer, len);

  op->slice = turbo_arena_buffer_slice(buffer, 0, len);
  if (!op->slice.data) {
    turbo_arena_buffer_unref(buffer);
    return_send_op(op);
    return -1;
  }

  /* TLOG_DEBUG("KCP output: %d bytes", len); */
  // printf("DEBUG: KCP output: %d bytes\n", len);

  uv_buf_t uv_buf = uv_buf_init(op->slice.data, (unsigned int)op->slice.length);
  int rc = uv_udp_send(&op->req, client->server->handle, &uv_buf, 1,
                       (const struct sockaddr *)&client->peer_addr, on_send_complete);

  turbo_arena_buffer_unref(buffer);

  if (rc != 0) {
    printf("DEBUG: uv_udp_send failed: %d\n", rc);
    return_send_op(op);
    return -1;
  }

  return 0;
}

/* Create KCP context with real ikcp */
static turbo_kcp_context_t *kcp_context_create(uint32_t conv_id, void *user) {
  turbo_kcp_context_t *ctx = malloc(sizeof(turbo_kcp_context_t));
  if (!ctx) return NULL;

  ctx->conv_id = conv_id;
  ctx->kcp = ikcp_create(conv_id, user);
  if (!ctx->kcp) {
    free(ctx);
    return NULL;
  }

  /* Set output callback */
  ikcp_setoutput(ctx->kcp, ikcp_output_callback);

  /* Apply configuration */
  int mtu = turbo_kcp_config_get_mtu();
  if (mtu > 0) {
    ikcp_setmtu(ctx->kcp, mtu);
  }

  int snd_wnd = turbo_kcp_config_get_snd_wnd();
  int rcv_wnd = turbo_kcp_config_get_rcv_wnd();
  if (snd_wnd > 0 && rcv_wnd > 0) {
    ikcp_wndsize(ctx->kcp, snd_wnd, rcv_wnd);
  }

  int nodelay = turbo_kcp_config_get_nodelay();
  int interval = turbo_kcp_config_get_interval();
  int resend = turbo_kcp_config_get_resend();
  int nc = turbo_kcp_config_get_nc();
  ikcp_nodelay(ctx->kcp, nodelay, interval, resend, nc);

  return ctx;
}

/* Destroy KCP context */
static void kcp_context_destroy(turbo_kcp_context_t *ctx) {
  if (!ctx) return;
  if (ctx->kcp) {
    ikcp_release(ctx->kcp);
  }
  free(ctx);
}

/* KCP update timer callback */
static void kcp_update_timer_cb(uv_timer_t *handle) {
  turbo_kcp_client_t *client = (turbo_kcp_client_t *)handle->data;
  if (!client || !client->kcp_ctx || !client->kcp_ctx->kcp) return;

  IUINT32 current = kcp_get_timestamp();
  ikcp_update(client->kcp_ctx->kcp, current);

  /* Schedule next update using ikcp_check for optimal timing */
  IUINT32 next = ikcp_check(client->kcp_ctx->kcp, current);
  IUINT32 delay = (next > current) ? (next - current) : 1;
  if (delay > 100) delay = 100; /* Cap at 100ms */

  uv_timer_start(&client->update_timer, kcp_update_timer_cb, delay, 0);
}

/* Receive buffer allocation */
static void alloc_recv_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  (void)suggested_size;

  turbo_kcp_server_t *server = (turbo_kcp_server_t *)handle->data;
  if (!server) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }

  /* Use ping-pong buffers */
  if (server->recv_buffer2 && server->recv_toggle == 0) {
    buf->base = server->recv_buffer2->data;
    buf->len = (unsigned int)server->recv_buffer2->capacity;
    server->recv_toggle = 1;
  } else if (server->recv_buffer1) {
    buf->base = server->recv_buffer1->data;
    buf->len = (unsigned int)server->recv_buffer1->capacity;
    server->recv_toggle = 0;
  } else {
    buf->base = NULL;
    buf->len = 0;
  }
}

/* Send raw UDP packet (for handshake) */
static int send_raw_packet(turbo_kcp_server_t *server, const struct sockaddr *addr,
                           const char *data, size_t len) {
  turbo_arena_buffer_t *buffer = turbo_arena_get_buffer(&server->arena, len);
  if (!buffer) return -1;

  memcpy(buffer->data, data, len);
  turbo_arena_buffer_set_used(buffer, len);

  turbo_kcp_send_op_t *op = get_send_op(server);
  if (!op) {
    turbo_arena_buffer_unref(buffer);
    return -1;
  }

  op->slice = turbo_arena_buffer_slice(buffer, 0, len);
  if (!op->slice.data) {
    turbo_arena_buffer_unref(buffer);
    return_send_op(op);
    return -1;
  }

  uv_buf_t uv_buf = uv_buf_init(op->slice.data, (unsigned int)op->slice.length);
  int rc = uv_udp_send(&op->req, server->handle, &uv_buf, 1, addr, on_send_complete);

  turbo_arena_buffer_unref(buffer);

  if (rc != 0) {
    return_send_op(op);
    return rc;
  }

  return 0;
}

/* Handle connection request (server-side) */
static void handle_connection_request(turbo_kcp_server_t *server, const struct sockaddr *addr,
                                      const char *data, size_t len) {
  if (len < sizeof(uint32_t) + strlen(KCP_CONN_SYN)) return;

  const char *msg = data + sizeof(uint32_t);
  if (strncmp(msg, KCP_CONN_SYN, strlen(KCP_CONN_SYN)) != 0) return;

  /* Create new client */
  turbo_kcp_client_t *client = turbo_arena_alloc(&server->arena, sizeof(turbo_kcp_client_t));
  if (!client) return;

  memset(client, 0, sizeof(*client));
  client->server = server;
  client->conv_id = turbo_kcp_alloc_conv_id();
  safe_copy_sockaddr(&client->peer_addr, addr);

  /* Create KCP context with real ikcp */
  client->kcp_ctx = kcp_context_create(client->conv_id, client);
  if (!client->kcp_ctx) return;

  /* Add to client mapping */
  add_client_mapping(client->conv_id, client);

  /* Send connection response */
  size_t response_size = sizeof(uint32_t) + strlen(KCP_CONN_ACK);
  char response[32];
  memcpy(response, &client->conv_id, sizeof(uint32_t));
  memcpy(response + sizeof(uint32_t), KCP_CONN_ACK, strlen(KCP_CONN_ACK));

  send_raw_packet(server, addr, response, response_size);

  /* Start client update timer */
  if (!client->timer_active) {
    uv_timer_init(server->loop, &client->update_timer);
    client->update_timer.data = client;
    uv_timer_start(&client->update_timer, kcp_update_timer_cb, 10, 0);
    client->timer_active = 1;
  }

  client->connected = 1;

  if (server->on_accept) {
    server->on_accept(server, client, NULL);
  }
}

/* Handle connection ACK (client-side) */
static int handle_connection_ack(turbo_kcp_server_t *server, const char *data, size_t len) {
  turbo_kcp_client_t *client = server->connecting_client;
  if (!client || !client->connecting) return 0;

  if (len < sizeof(uint32_t) + strlen(KCP_CONN_ACK)) return 0;

  uint32_t conv_id;
  memcpy(&conv_id, data, sizeof(uint32_t));

  const char *msg = data + sizeof(uint32_t);
  if (strncmp(msg, KCP_CONN_ACK, strlen(KCP_CONN_ACK)) != 0) return 0;

  /* Update client with received conv_id */
  client->conv_id = conv_id;
  client->connecting = 0;
  client->connected = 1;
  server->connecting_client = NULL;

  /* Recreate KCP context with correct conv_id */
  if (client->kcp_ctx) {
    kcp_context_destroy(client->kcp_ctx);
  }
  client->kcp_ctx = kcp_context_create(conv_id, client);
  if (!client->kcp_ctx) {
    if (client->on_connect) {
      client->on_connect(client, UV_ENOMEM, NULL);
    }
    return 1;
  }

  /* Start client update timer */
  if (!client->timer_active) {
    uv_timer_init(server->loop, &client->update_timer);
    client->update_timer.data = client;
    uv_timer_start(&client->update_timer, kcp_update_timer_cb, 10, 0);
    client->timer_active = 1;
  }

  if (client->on_connect) {
    client->on_connect(client, 0, NULL);
  }
  
  /* Register client in global map so on_kcp_recv can find it by conv_id */
  add_client_mapping(conv_id, client);
  
  return 1;
}

/* Process received data and deliver to application */
static void process_kcp_recv(turbo_kcp_server_t *server, turbo_kcp_client_t *client) {
  if (!client || !client->kcp_ctx || !client->kcp_ctx->kcp) return;

  int peeksize;
  while ((peeksize = ikcp_peeksize(client->kcp_ctx->kcp)) > 0) {
    turbo_arena_buffer_t *buffer = turbo_arena_get_buffer(&server->arena, peeksize);
    if (!buffer) break;

    int recv_len = ikcp_recv(client->kcp_ctx->kcp, buffer->data, peeksize);
    if (recv_len > 0) {
      turbo_arena_buffer_set_used(buffer, recv_len);

      if (server->on_recv) {
        turbo_arena_slice_t slice = turbo_arena_buffer_slice(buffer, 0, recv_len);
        server->on_recv(server, &slice, client);
        turbo_arena_slice_release(&slice);
      }
    }

    turbo_arena_buffer_unref(buffer);

    /* Check if client was closed in the callback */
    if (!client->kcp_ctx) {
      break;
    }
  }
}

/* Receive callback */
static void on_kcp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
  (void)flags;

  turbo_kcp_server_t *server = (turbo_kcp_server_t *)handle->data;
  if (!server) return;

  if (nread < 0) {
    turbo_stats_counter_inc_fast(s_kcp_stats.recv_errors);
    return;
  }

  if (nread == 0 || !buf || !buf->base || !addr) return;

  turbo_stats_counter_add_fast(s_kcp_stats.bytes_received, (size_t)nread);

  /* Client-mode: check for connection ACK first */
  if (server->connecting_client && handle_connection_ack(server, buf->base, nread)) {
    return;
  }

  /* Check for connection request (conv_id == 0) */
  if (nread >= (ssize_t)sizeof(uint32_t)) {
    uint32_t first_word;
    memcpy(&first_word, buf->base, sizeof(uint32_t));

    if (first_word == KCP_CONN_REQUEST) {
      handle_connection_request(server, addr, buf->base, nread);
      return;
    }

    /* Get conv_id from KCP packet header */
    IUINT32 conv_id = ikcp_getconv(buf->base);

    /* Find client by conv_id */
    turbo_kcp_client_t *client = find_client_by_conv_id(conv_id);

    /* Also check client-mode client */
    if (!client && server->connecting_client &&
        server->connecting_client->conv_id == conv_id) {
      client = server->connecting_client;
    }

    if (client && client->kcp_ctx && client->kcp_ctx->kcp) {
      /* Feed data to ikcp */
      int rc = ikcp_input(client->kcp_ctx->kcp, buf->base, (long)nread);
      if (rc < 0) {
        turbo_stats_counter_inc_fast(s_kcp_stats.recv_errors);
        return;
      }

      /* Process received data */
      process_kcp_recv(server, client);
    }
  }
}

/* Handle close callback */
static void on_handle_closed(uv_handle_t *handle) {
  free(handle);
}

/* Initialize KCP server */
int turbo_kcp_server_init(turbo_kcp_server_t *server, uv_loop_t *loop, const char *host,
                          unsigned short port) {
  if (!server || !loop) return UV_EINVAL;

  memset(server, 0, sizeof(*server));
  server->loop = loop;

  init_kcp_stats();

  if (turbo_arena_init(&server->arena, 0) != 0) {
    return UV_ENOMEM;
  }

  server->handle = (uv_udp_t *)malloc(sizeof(uv_udp_t));
  if (!server->handle) {
    turbo_arena_free(&server->arena);
    return UV_ENOMEM;
  }

  int rc = uv_udp_init(loop, server->handle);
  if (rc != 0) {
    free(server->handle);
    turbo_arena_free(&server->arena);
    return rc;
  }

  server->handle->data = server;

  /* Allocate receive buffers */
  size_t recv_buf_size = TURBO_KCP_DEFAULT_RECV_BUFFER_SIZE;
  server->recv_buffer1 = turbo_arena_get_buffer(&server->arena, recv_buf_size);
  server->recv_buffer2 = turbo_arena_get_buffer(&server->arena, recv_buf_size);

  if (!server->recv_buffer1) {
    uv_close((uv_handle_t *)server->handle, on_handle_closed);
    turbo_arena_free(&server->arena);
    return UV_ENOMEM;
  }

  server->recv_toggle = 0;

  /* Bind to address */
  struct sockaddr_in addr4;
  const char *bind_host = host ? host : "0.0.0.0";
  rc = uv_ip4_addr(bind_host, (int)port, &addr4);
  if (rc != 0) {
    uv_close((uv_handle_t *)server->handle, on_handle_closed);
    turbo_arena_free(&server->arena);
    return rc;
  }

  rc = uv_udp_bind(server->handle, (const struct sockaddr *)&addr4, 0);
  if (rc != 0) {
    uv_close((uv_handle_t *)server->handle, on_handle_closed);
    turbo_arena_free(&server->arena);
    return rc;
  }


  return 0;
}

/* Start KCP server */
int turbo_kcp_server_start(turbo_kcp_server_t *server, turbo_accept_cb accept_cb,
                           turbo_recv_cb recv_cb) {
  if (!server || !server->handle) return UV_EINVAL;

  server->on_accept = accept_cb;
  server->on_recv = recv_cb;
  return uv_udp_recv_start(server->handle, alloc_recv_buffer, on_kcp_recv);
}

/* Stop KCP server */
void turbo_kcp_server_stop(turbo_kcp_server_t *server) {
  if (!server) return;

  if (server->handle) {
    uv_udp_recv_stop(server->handle);
    if (!uv_is_closing((uv_handle_t *)server->handle)) {
      uv_close((uv_handle_t *)server->handle, on_handle_closed);
    }
    server->handle = NULL;
  }

  /* Clean up client mappings */
  for (int i = 0; i < KCP_CLIENT_MAP_SIZE; i++) {
    turbo_kcp_client_mapping_t *mapping = g_client_map[i];
    while (mapping) {
      turbo_kcp_client_mapping_t *next = mapping->next;

      if (mapping->client && mapping->client->timer_active) {
        uv_timer_stop(&mapping->client->update_timer);
        uv_close((uv_handle_t *)&mapping->client->update_timer, NULL);
        mapping->client->timer_active = 0;
      }

      if (mapping->client && mapping->client->kcp_ctx) {
        kcp_context_destroy(mapping->client->kcp_ctx);
        mapping->client->kcp_ctx = NULL;
      }

      free(mapping);
      mapping = next;
    }
    g_client_map[i] = NULL;
  }

  if (server->recv_buffer1) {
    turbo_arena_buffer_unref(server->recv_buffer1);
    server->recv_buffer1 = NULL;
  }
  if (server->recv_buffer2) {
    turbo_arena_buffer_unref(server->recv_buffer2);
    server->recv_buffer2 = NULL;
  }

  turbo_arena_free(&server->arena);
}

/* Set KCP nodelay configuration */
int turbo_kcp_server_set_nodelay(turbo_kcp_server_t *server, int nodelay, int interval,
                                 int resend, int nc) {
  if (!server) return UV_EINVAL;

  turbo_kcp_config_set_nodelay(nodelay);
  turbo_kcp_config_set_interval(interval);
  turbo_kcp_config_set_resend(resend);
  turbo_kcp_config_set_nc(nc);

  return 0;
}

/* Set KCP window size */
int turbo_kcp_server_set_wndsize(turbo_kcp_server_t *server, int sndwnd, int rcvwnd) {
  if (!server) return UV_EINVAL;

  turbo_kcp_config_set_snd_wnd(sndwnd);
  turbo_kcp_config_set_rcv_wnd(rcvwnd);

  return 0;
}

/* Set KCP MTU */
int turbo_kcp_server_set_mtu(turbo_kcp_server_t *server, int mtu) {
  if (!server || mtu < 50 || mtu > 65535) return UV_EINVAL;

  turbo_kcp_config_set_mtu(mtu);

  return 0;
}

/* Get zero-copy send buffer */
turbo_arena_buffer_t *turbo_kcp_get_send_buffer(turbo_kcp_server_t *server, size_t min_size) {
  if (!server) return NULL;
  return turbo_arena_get_pooled_buffer(&server->arena, min_size);
}

/* Send with zero-copy buffer */
int turbo_kcp_send_buffer(turbo_kcp_client_t *client, turbo_arena_buffer_t *buffer,
                          size_t length) {
  if (!client || !client->kcp_ctx || !client->kcp_ctx->kcp || !buffer) return UV_EINVAL;
  if (length > buffer->used) return UV_EINVAL;

  int rc = ikcp_send(client->kcp_ctx->kcp, buffer->data, (int)length);
  if (rc < 0) {
    turbo_stats_counter_inc_fast(s_kcp_stats.send_errors);
    return UV_EIO;
  }

  /* Flush for low latency */
  ikcp_flush(client->kcp_ctx->kcp);

  return 0;
}

/* Send with copy */
int turbo_kcp_send(turbo_kcp_client_t *client, const char *data, size_t length) {
  if (!client || !client->kcp_ctx || !client->kcp_ctx->kcp || !data || length == 0)
    return UV_EINVAL;

  int rc = ikcp_send(client->kcp_ctx->kcp, data, (int)length);
  if (rc < 0) {
    turbo_stats_counter_inc_fast(s_kcp_stats.send_errors);
    return UV_EIO;
  }

  /* Flush for low latency */
  ikcp_flush(client->kcp_ctx->kcp);
  
  return 0;
}

/* Initialize KCP client */
int turbo_kcp_client_init(turbo_kcp_client_t *client, uv_loop_t *loop) {
  if (!client || !loop) return UV_EINVAL;

  memset(client, 0, sizeof(*client));
  init_kcp_stats();

  client->server = malloc(sizeof(turbo_kcp_server_t));
  if (!client->server) return UV_ENOMEM;

  memset(client->server, 0, sizeof(*client->server));
  client->server->loop = loop;

  if (turbo_arena_init(&client->server->arena, 0) != 0) {
    free(client->server);
    return UV_ENOMEM;
  }

  client->server->handle = (uv_udp_t *)malloc(sizeof(uv_udp_t));
  if (!client->server->handle) {
    turbo_arena_free(&client->server->arena);
    free(client->server);
    return UV_ENOMEM;
  }

  int rc = uv_udp_init(loop, client->server->handle);
  if (rc != 0) {
    free(client->server->handle);
    turbo_arena_free(&client->server->arena);
    free(client->server);
    return rc;
  }

  client->server->handle->data = client->server;
  return 0;
}

/* Connect KCP client */
int turbo_kcp_client_connect(turbo_kcp_client_t *client, const char *host, unsigned short port,
                             turbo_connect_cb connect_cb, turbo_recv_cb recv_cb) {
  if (!client || !client->server || !host) return UV_EINVAL;

  client->on_connect = connect_cb;
  client->server->on_recv = recv_cb;

  struct sockaddr_in addr4;
  int rc = uv_ip4_addr(host, (int)port, &addr4);
  if (rc != 0) return rc;

  safe_copy_sockaddr(&client->peer_addr, (const struct sockaddr *)&addr4);

  /* Bind client UDP socket */
  struct sockaddr_in local_addr;
  rc = uv_ip4_addr("0.0.0.0", 0, &local_addr);
  if (rc != 0) return rc;

  rc = uv_udp_bind(client->server->handle, (const struct sockaddr *)&local_addr, 0);
  if (rc != 0) return rc;

  /* Allocate receive buffer */
  size_t recv_buf_size = TURBO_KCP_DEFAULT_RECV_BUFFER_SIZE;
  client->server->recv_buffer1 = turbo_arena_get_buffer(&client->server->arena, recv_buf_size);
  if (!client->server->recv_buffer1) {
    return UV_ENOMEM;
  }
  client->server->recv_toggle = 0;

  /* Start receiving before sending SYN */
  rc = uv_udp_recv_start(client->server->handle, alloc_recv_buffer, on_kcp_recv);
  if (rc != 0) return rc;

  /* Create temporary KCP context (will be recreated with correct conv_id) */
  client->kcp_ctx = kcp_context_create(0, client);
  if (!client->kcp_ctx) {
    return UV_ENOMEM;
  }

  client->connecting = 1;
  client->is_client_mode = 1;
  client->server->connecting_client = client;

  /* Send connection request */
  size_t request_size = sizeof(uint32_t) + strlen(KCP_CONN_SYN);
  char request[32];
  uint32_t req_conv_id = KCP_CONN_REQUEST;
  memcpy(request, &req_conv_id, sizeof(uint32_t));
  memcpy(request + sizeof(uint32_t), KCP_CONN_SYN, strlen(KCP_CONN_SYN));

  rc = send_raw_packet(client->server, (const struct sockaddr *)&addr4, request, request_size);
  if (rc != 0) return rc;

  return 0;
}

/* Close KCP client */
void turbo_kcp_client_close(turbo_kcp_client_t *client) {
  if (!client) return;

  /* Check if we are closing a copy (e.g. from turbo_coro_client).
     If so, find the original client and close IT instead. */
  if (client->conv_id != 0) {
    turbo_kcp_client_t *original = find_client_by_conv_id(client->conv_id);
    if (original && original != client) {
      turbo_kcp_client_close(original);
      
      /* Clear the copy's pointers so we don't double-free later */
      client->kcp_ctx = NULL;
      client->server = NULL;
      client->timer_active = 0;
      return;
    }
  }

  if (client->timer_active) {
    uv_timer_stop(&client->update_timer);
    uv_close((uv_handle_t *)&client->update_timer, NULL);
    client->timer_active = 0;
  }

  if (client->kcp_ctx) {
    kcp_context_destroy(client->kcp_ctx);
    client->kcp_ctx = NULL;
  }

  if (client->conv_id != 0) {
    remove_client_mapping(client->conv_id);
  }

  if (client->server) {
    /* Only close the server handle and free the server structure if we are a standalone client.
       Server-side clients share the server structure which is managed by turbo_kcp_server_stop. */
    if (client->is_client_mode) {
      if (client->server->handle) {
        uv_udp_recv_stop(client->server->handle);
        if (!uv_is_closing((uv_handle_t *)client->server->handle)) {
          uv_close((uv_handle_t *)client->server->handle, on_handle_closed);
        }
      }
      turbo_arena_free(&client->server->arena);
      free(client->server);
    }
    client->server = NULL;
  }

  client->connected = 0;
}

/* Client send operations */
int turbo_kcp_client_send(turbo_kcp_client_t *client, const char *data, size_t length) {
  return turbo_kcp_send(client, data, length);
}

int turbo_kcp_client_send_buffer(turbo_kcp_client_t *client, turbo_arena_buffer_t *buffer,
                                 size_t length) {
  return turbo_kcp_send_buffer(client, buffer, length);
}

int turbo_kcp_client_sendv(turbo_kcp_client_t *client, const turbo_kcp_iovec_t *iov,
                           size_t iovcnt) {
  if (!client || !iov || iovcnt == 0) return UV_EINVAL;

  int rc = 0;
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      rc = turbo_kcp_send(client, iov[i].data, iov[i].len);
    }
  }

  if (rc == 0) {
    TURBO_STATS_INC("kcp.scatter_gather_sends");
  }

  return rc;
}

/* Get server statistics */
void turbo_kcp_get_stats(const turbo_kcp_server_t *server, turbo_kcp_stats_t *stats) {
  if (!server || !stats) return;

  memset(stats, 0, sizeof(*stats));
  turbo_arena_get_stats(&server->arena, &stats->arena_stats);

  turbo_stat_entry_t *entry;

  entry = turbo_stats_get("kcp.bytes_sent");
  stats->bytes_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.bytes_received");
  stats->bytes_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.packets_sent");
  stats->packets_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.packets_received");
  stats->packets_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.messages_sent");
  stats->messages_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.messages_received");
  stats->messages_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.zero_copy_sends");
  stats->zero_copy_sends = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.copy_sends");
  stats->copy_sends = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.connections_established");
  stats->connections_established = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.send_errors");
  stats->send_errors = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.recv_errors");
  stats->recv_errors = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.updates");
  stats->kcp_updates = entry ? entry->data.counter : 0;
}

/* Get client statistics */
void turbo_kcp_client_get_stats(const turbo_kcp_client_t *client, turbo_kcp_stats_t *stats) {
  if (!client || !stats) return;

  memset(stats, 0, sizeof(*stats));

  turbo_stat_entry_t *entry;

  entry = turbo_stats_get("kcp.bytes_sent");
  stats->bytes_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.bytes_received");
  stats->bytes_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.packets_sent");
  stats->packets_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.packets_received");
  stats->packets_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.messages_sent");
  stats->messages_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.messages_received");
  stats->messages_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.zero_copy_sends");
  stats->zero_copy_sends = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.copy_sends");
  stats->copy_sends = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.connections_established");
  stats->connections_established = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.send_errors");
  stats->send_errors = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.recv_errors");
  stats->recv_errors = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("kcp.updates");
  stats->kcp_updates = entry ? entry->data.counter : 0;

  stats->clients_active = client->connected ? 1 : 0;

  if (client->server) {
    turbo_arena_get_stats(&client->server->arena, &stats->arena_stats);
  }
}

/* Reset server statistics */
void turbo_kcp_reset_stats(turbo_kcp_server_t *server) {
  if (!server) return;

  turbo_stats_reset("kcp.bytes_sent");
  turbo_stats_reset("kcp.bytes_received");
  turbo_stats_reset("kcp.packets_sent");
  turbo_stats_reset("kcp.packets_received");
  turbo_stats_reset("kcp.messages_sent");
  turbo_stats_reset("kcp.messages_received");
  turbo_stats_reset("kcp.zero_copy_sends");
  turbo_stats_reset("kcp.copy_sends");
  turbo_stats_reset("kcp.connections_established");
  turbo_stats_reset("kcp.send_errors");
  turbo_stats_reset("kcp.recv_errors");
  turbo_stats_reset("kcp.updates");
  turbo_stats_reset("kcp.send_ops_allocated");
  turbo_stats_reset("kcp.send_ops_reused");
  turbo_stats_reset("kcp.send_ops_pooled");
  turbo_stats_reset("kcp.send_ops_freed");
  turbo_stats_reset("kcp.clients_mapped");
  turbo_stats_reset("kcp.clients_unmapped");
  turbo_stats_reset("kcp.kcp_outputs");
  turbo_stats_reset("kcp.input_errors");
}

/* Trim arena memory */
void turbo_kcp_trim_memory(turbo_kcp_server_t *server) {
  if (!server) return;
  turbo_arena_trim(&server->arena);
}

/* Get memory usage */
size_t turbo_kcp_get_memory_usage(const turbo_kcp_server_t *server) {
  if (!server) return 0;

  turbo_arena_stats_t stats;
  turbo_arena_get_stats(&server->arena, &stats);
  return stats.total_allocated;
}

/* Cleanup global pools */
void turbo_kcp_cleanup_pools(void) {
  while (g_send_op_pool) {
    turbo_kcp_send_op_t *op = g_send_op_pool;
    g_send_op_pool = op->next;
    free(op);
  }
  g_send_op_pool_size = 0;

  for (int i = 0; i < KCP_CLIENT_MAP_SIZE; i++) {
    turbo_kcp_client_mapping_t *mapping = g_client_map[i];
    while (mapping) {
      turbo_kcp_client_mapping_t *next = mapping->next;
      free(mapping);
      mapping = next;
    }
    g_client_map[i] = NULL;
  }

  TURBO_STATS_INC("kcp.pools_cleaned");
}
