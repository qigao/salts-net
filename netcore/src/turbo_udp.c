#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#include "config.h"
#include "arena_buffer.h"
#include "stats.h"
#include "turbo_udp.h"
#include "internal.h"
#include "tlog.h"
 

/* Forward declarations for pool synchronization */
extern void turbo_udp_pool_lock(void);
extern void turbo_udp_pool_unlock(void);

/* Send operation for zero-copy */
typedef struct turbo_udp_send_op_s {
  uv_udp_send_t req;
  turbo_arena_slice_t slice;
  turbo_udp_server_t *server;
  struct turbo_udp_send_op_s *next;
} turbo_udp_send_op_t;

/* Send operation pool */
static turbo_udp_send_op_t *g_send_op_pool = NULL;
static size_t g_send_op_pool_size = 0;
static const size_t MAX_SEND_OP_POOL_SIZE = 512;

/* UDP Statistics IDs */
static struct {
    turbo_stat_id_t bytes_sent;
    turbo_stat_id_t bytes_received;
    turbo_stat_id_t send_errors;
    turbo_stat_id_t recv_errors;
    int initialized;
} s_udp_stats = {0};

static void init_udp_stats(void) {
    if (s_udp_stats.initialized) return;
    
    s_udp_stats.bytes_sent = turbo_stats_register("udp.bytes_sent", TURBO_STAT_COUNTER);
    s_udp_stats.bytes_received = turbo_stats_register("udp.bytes_received", TURBO_STAT_COUNTER);
    s_udp_stats.send_errors = turbo_stats_register("udp.send_errors", TURBO_STAT_COUNTER);
    s_udp_stats.recv_errors = turbo_stats_register("udp.recv_errors", TURBO_STAT_COUNTER);
    
    s_udp_stats.initialized = 1;
}

/* Get send operation from pool (thread-safe) */
static turbo_udp_send_op_t *get_send_op(turbo_udp_server_t *server) {
  turbo_udp_send_op_t *op = NULL;

  turbo_udp_pool_lock();
  if (g_send_op_pool && g_send_op_pool_size > 0) {
    op = g_send_op_pool;
    g_send_op_pool = op->next;
    g_send_op_pool_size--;
  }
  turbo_udp_pool_unlock();

  if (!op) {
    op = (turbo_udp_send_op_t *)malloc(sizeof(turbo_udp_send_op_t));
    if (op) {
    } else {
      return NULL; // NULL check
    }
  }

  memset(op, 0, sizeof(*op));
  op->server = server;

  return op;
}

/* Return send operation to pool (thread-safe) */
static void return_send_op(turbo_udp_send_op_t *op) {
  if (!op)
    return;

  /* Release the slice */
  turbo_arena_slice_release(&op->slice);

  turbo_udp_pool_lock();
  if (g_send_op_pool_size < MAX_SEND_OP_POOL_SIZE) {
    op->next = g_send_op_pool;
    g_send_op_pool = op;
    g_send_op_pool_size++;
    turbo_udp_pool_unlock();
    turbo_udp_pool_unlock();
  } else {
    turbo_udp_pool_unlock();
    free(op);
  }
}

/* Send completion callback */
static void on_send_complete(uv_udp_send_t *req, int status) {
  turbo_udp_send_op_t *op =
      (turbo_udp_send_op_t *)((char *)req - offsetof(turbo_udp_send_op_t, req));

  if (status == 0) {
    turbo_stats_counter_add_fast(s_udp_stats.bytes_sent, op->slice.length);
  } else {
    turbo_stats_counter_inc_fast(s_udp_stats.send_errors);
  }

  return_send_op(op);
}

/* Receive buffer allocation */
static void alloc_recv_buffer(uv_handle_t *handle, size_t suggested_size,
                              uv_buf_t *buf) {
  (void)suggested_size;

  turbo_udp_server_t *server = (turbo_udp_server_t *)handle->data;
  if (!server) {
    buf->base = NULL;
    buf->len = 0;
    return;
  }

  /* Use ping-pong buffers for zero-copy receives */
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

/* Receive callback */
static void on_udp_recv(uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf,
                        const struct sockaddr *addr, unsigned flags) {
  (void)flags;

  turbo_udp_server_t *server = (turbo_udp_server_t *)handle->data;
  if (!server)
    return;

  if (nread < 0) {
    turbo_stats_counter_inc_fast(s_udp_stats.recv_errors);
    return;
  }

  if (nread == 0 || !buf || !buf->base || !addr)
    return;

  turbo_stats_counter_add_fast(s_udp_stats.bytes_received, (size_t)nread);

  /* Determine which buffer was used */
  turbo_arena_buffer_t *used_buffer = NULL;
  if (server->recv_buffer1 && buf->base == server->recv_buffer1->data) {
    used_buffer = server->recv_buffer1;
  } else if (server->recv_buffer2 && buf->base == server->recv_buffer2->data) {
    used_buffer = server->recv_buffer2;
  }

  if (used_buffer && server->on_recv) {
    /* Set the used size in the buffer */
    turbo_arena_buffer_set_used(used_buffer, (size_t)nread);

    /* Create zero-copy slice for the received data */
    turbo_arena_slice_t slice =
        turbo_arena_buffer_slice(used_buffer, 0, (size_t)nread);

    /* Call user callback with zero-copy slice */
    server->on_recv(server, &slice, (void *)addr);

    /* Check if server was destroyed during callback */
    if (handle->data == NULL) {
      return;
    }

    /* Release the slice (user should have ref'd it if needed) */
    turbo_arena_slice_release(&slice);
  }
}

/* Handle close callback */
static void on_handle_closed(uv_handle_t *handle) { free(handle); }

/* Initialize enhanced UDP server */
int turbo_udp_server_init(turbo_udp_server_t *server, uv_loop_t *loop,
                         const char *host, unsigned short port) {
  if (!server || !loop)
    return UV_EINVAL;

  memset(server, 0, sizeof(*server));
  server->loop = loop;
  
  init_udp_stats();

  /* Initialize arena */
  if (turbo_arena_init(&server->arena, 0) != 0) {
    return UV_ENOMEM;
  }

  /* Create UDP handle */
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

  /* Allocate receive buffers from arena */
  size_t recv_buf_size = turbo_udp_config_get_recv_buf_size();
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
} /* S
 tart enhanced UDP server */
int turbo_udp_server_start(turbo_udp_server_t *server, turbo_recv_cb cb) {
  if (!server || !server->handle)
    return UV_EINVAL;

  server->on_recv = cb;
  int rc = uv_udp_recv_start(server->handle, alloc_recv_buffer, on_udp_recv);
  if (rc == 0) {
    TLOG_INFO("UDP server listening");
  } else {
    TLOG_ERROR("UDP server start failed: {:s}", uv_strerror(rc));
  }
  return rc;
}

/* Stop enhanced UDP server */
void turbo_udp_server_stop(turbo_udp_server_t *server) {
  if (!server)
    return;

  if (server->handle) {
    uv_udp_recv_stop(server->handle);
    server->handle->data = NULL; /* Signal callbacks that server is stopping */
    if (!uv_is_closing((uv_handle_t *)server->handle)) {
      uv_close((uv_handle_t *)server->handle, on_handle_closed);
    }
    server->handle = NULL;
  }

  /* Release receive buffers */
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

/* Get zero-copy send buffer */
turbo_arena_buffer_t *turbo_udp_get_send_buffer(turbo_udp_server_t *server,
                                              size_t min_size) {
  if (!server)
    return NULL;

  return turbo_arena_get_pooled_buffer(&server->arena, min_size);
}

/* Send with zero-copy buffer */
int turbo_udp_send_buffer(turbo_udp_server_t *server, const struct sockaddr *dest,
                         turbo_arena_buffer_t *buffer, size_t length) {
  if (!server || !server->handle || !dest || !buffer)
    return UV_EINVAL;

  if (length > buffer->used)
    return UV_EINVAL;

  turbo_udp_send_op_t *op = get_send_op(server);
  if (!op)
    return UV_ENOMEM;

  /* Create slice from buffer */
  op->slice = turbo_arena_buffer_slice(buffer, 0, length);
  if (!op->slice.data) {
    return_send_op(op);
    return UV_ENOMEM;
  }

  /* Setup libuv buffer */
  uv_buf_t uv_buf = uv_buf_init(op->slice.data, (unsigned int)op->slice.length);

  /* Send the data */
  int rc =
      uv_udp_send(&op->req, server->handle, &uv_buf, 1, dest, on_send_complete);
  if (rc != 0) {
    return_send_op(op);
    return rc;
  }


  return 0;
}

/* Send with automatic buffer allocation and copy (fallback) */
int turbo_udp_send(turbo_udp_server_t *server, const struct sockaddr *dest,
                  const char *data, size_t length) {
  if (!server || !dest || !data || length == 0)
    return UV_EINVAL;

  /* Get buffer from arena */
  turbo_arena_buffer_t *buffer = turbo_udp_get_send_buffer(server, length);
  if (!buffer)
    return UV_ENOMEM;

  /* Copy data to buffer */
  memcpy(buffer->data, data, length);
  turbo_arena_buffer_set_used(buffer, length);

  /* Send the buffer */
  int rc = turbo_udp_send_buffer(server, dest, buffer, length);

  /* Release our reference to the buffer */
  turbo_arena_buffer_unref(buffer);

  if (rc == 0) {
    
  }

  return rc;
}

/* Connect for client-style usage */
int turbo_udp_connect(turbo_udp_client_t *client, const char *host,
                     unsigned short port) {
  if (!client || !client->handle)
    return UV_EINVAL;

  struct sockaddr_in addr4;
  int rc = uv_ip4_addr(host ? host : "127.0.0.1", (int)port, &addr4);
  if (rc != 0)
    return rc;

  return uv_udp_connect(client->handle, (const struct sockaddr *)&addr4);
}

/* Send to connected peer */
int turbo_udp_send_connected(turbo_udp_client_t *client, const char *data,
                            size_t length) {
  return turbo_udp_send((turbo_udp_server_t *)client, NULL, data, length);
}

/* Send buffer to connected peer */
int turbo_udp_send_buffer_connected(turbo_udp_client_t *client,
                                   turbo_arena_buffer_t *buffer, size_t length) {
  return turbo_udp_send_buffer((turbo_udp_server_t *)client, NULL, buffer,
                              length);
}

int turbo_udp_sendv_connected(turbo_udp_client_t *client, const turbo_udp_iovec_t *iov, size_t iovcnt) {
  if (!client || !iov || iovcnt == 0)
    return UV_EINVAL;

  int rc = 0;

  /* Send all buffers using zero-copy wrappers (NO MEMCPY!) */
  for (size_t i = 0; i < iovcnt && rc == 0; i++) {
    if (iov[i].len > 0 && iov[i].data) {
      /* Wrap user buffer - ZERO COPY! */
      turbo_arena_buffer_t *buffer = turbo_arena_wrap_external(
          (void*)iov[i].data,
          iov[i].len,
          NULL,  /* No free callback - user manages memory */
          NULL
      );
      if (!buffer) {
        rc = UV_ENOMEM;
        break;
      }

      rc = turbo_udp_send_buffer_connected(client, buffer, iov[i].len);
      turbo_arena_buffer_unref(buffer);
    }
  }

  if (rc == 0) {
    
  }

  return rc;
}

/* Get server statistics */
void turbo_udp_get_stats(const turbo_udp_server_t *server,
                        turbo_udp_stats_t *stats) {
  if (!server || !stats)
    return;

  memset(stats, 0, sizeof(*stats));

  /* Get arena statistics */
  turbo_arena_get_stats(&server->arena, &stats->arena_stats);

  /* Get global UDP statistics from stats system */
  turbo_stat_entry_t *entry;

  entry = turbo_stats_get("udp.bytes_sent");
  stats->bytes_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.bytes_received");
  stats->bytes_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.packets_sent");
  stats->packets_sent = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.packets_received");
  stats->packets_received = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.zero_copy_sends");
  stats->zero_copy_sends = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.copy_sends");
  stats->copy_sends = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.send_errors");
  stats->send_errors = entry ? entry->data.counter : 0;

  entry = turbo_stats_get("udp.recv_errors");
  stats->recv_errors = entry ? entry->data.counter : 0;
}

/* Reset server statistics */
void turbo_udp_reset_stats(turbo_udp_server_t *server) {
  if (!server)
    return;

  /* Reset UDP-specific stats */
  turbo_stats_reset("udp.bytes_sent");
  turbo_stats_reset("udp.bytes_received");
  turbo_stats_reset("udp.packets_sent");
  turbo_stats_reset("udp.packets_received");
  turbo_stats_reset("udp.zero_copy_sends");
  turbo_stats_reset("udp.copy_sends");
  turbo_stats_reset("udp.send_errors");
  turbo_stats_reset("udp.recv_errors");
  turbo_stats_reset("udp.send_ops_allocated");
  turbo_stats_reset("udp.send_ops_reused");
  turbo_stats_reset("udp.send_ops_pooled");
  turbo_stats_reset("udp.send_ops_freed");
}

/* Trim arena memory */
void turbo_udp_trim_memory(turbo_udp_server_t *server) {
  if (!server)
    return;

  turbo_arena_trim(&server->arena);
}

/* Get memory usage */
size_t turbo_udp_get_memory_usage(const turbo_udp_server_t *server) {
  if (!server)
    return 0;

  turbo_arena_stats_t stats;
  turbo_arena_get_stats(&server->arena, &stats);
  return stats.total_allocated;
}

/* Cleanup global pools */
void turbo_udp_cleanup_pools(void) {
  /* Cleanup send operation pool */
  while (g_send_op_pool) {
    turbo_udp_send_op_t *op = g_send_op_pool;
    g_send_op_pool = op->next;
    free(op);
  }
  g_send_op_pool_size = 0;

  g_send_op_pool_size = 0;
}

/* ========================================================================
 * Multicast Operations
 * ======================================================================== */

/**
 * @brief Join a UDP multicast group
 * @param udp UDP socket
 * @param multicast_addr Multicast group address (e.g., "239.0.0.1")
 * @param interface_addr Interface address (NULL for default)
 * @return 0 on success, negative error code on failure
 */
int turbo_udp_join_multicast_group(turbo_udp_t* udp, const char* multicast_addr, 
                                   const char* interface_addr) {
    if (!udp || !udp->handle || !multicast_addr)
        return UV_EINVAL;
    
    int rc = uv_udp_set_membership(udp->handle, multicast_addr, interface_addr, UV_JOIN_GROUP);
    if (rc == 0) {
        TLOG_INFO("Joined multicast group: {:s}", multicast_addr);
    } else {
        turbo_stats_counter_inc_fast(s_udp_stats.recv_errors);
        TLOG_DEBUG("Failed to join multicast group {:s}: {:s}", multicast_addr, uv_strerror(rc));
    }
    
    return rc;
}

/**
 * @brief Leave a UDP multicast group
 * @param udp UDP socket
 * @param multicast_addr Multicast group address
 * @param interface_addr Interface address (NULL for default)
 * @return 0 on success, negative error code on failure
 */
int turbo_udp_leave_multicast_group(turbo_udp_t* udp, const char* multicast_addr,
                                    const char* interface_addr) {
    if (!udp || !udp->handle || !multicast_addr)
        return UV_EINVAL;
    
    int rc = uv_udp_set_membership(udp->handle, multicast_addr, interface_addr, UV_LEAVE_GROUP);
    if (rc == 0) {
        
    } else {
        turbo_stats_counter_inc_fast(s_udp_stats.recv_errors);
    }
    
    return rc;
}

/**
 * @brief Enable/disable multicast loopback
 * @param udp UDP socket
 * @param on 1 to enable, 0 to disable
 * @return 0 on success, negative error code on failure
 */
int turbo_udp_set_multicast_loop(turbo_udp_t* udp, int on) {
    if (!udp || !udp->handle)
        return UV_EINVAL;
    
    return uv_udp_set_multicast_loop(udp->handle, on);
}

/**
 * @brief Set multicast TTL (time-to-live)
 * @param udp UDP socket
 * @param ttl TTL value (1-255)
 * @return 0 on success, negative error code on failure
 */
int turbo_udp_set_multicast_ttl(turbo_udp_t* udp, int ttl) {
    if (!udp || !udp->handle)
        return UV_EINVAL;
    
    if (ttl < 1 || ttl > 255)
        return UV_EINVAL;
    
    return uv_udp_set_multicast_ttl(udp->handle, ttl);
}

/**
 * @brief Enable/disable broadcast
 * @param udp UDP socket
 * @param on 1 to enable, 0 to disable
 * @return 0 on success, negative error code on failure
 */
int turbo_udp_set_broadcast(turbo_udp_t* udp, int on) {
    if (!udp || !udp->handle)
        return UV_EINVAL;
    
    return uv_udp_set_broadcast(udp->handle, on);
}
