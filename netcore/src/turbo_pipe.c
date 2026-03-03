#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "turbo_pipe.h"
#include "stats.h"
#include "config.h"
#include "internal.h"
#include "tlog.h"

/* Enhanced Pipe with True Zero-Copy Implementation */

/* Forward declarations for global synchronization */
extern void turbo_pipe_sync_lock(void);
extern void turbo_pipe_sync_unlock(void);

/* Send operation for zero-copy */
typedef struct turbo_pipe_send_op_s {
    uv_write_t req;
    turbo_arena_slice_t* slices;
    size_t slice_count;
    turbo_pipe_client_t* client;
    struct turbo_pipe_send_op_s* next;
} turbo_pipe_send_op_t;

/* Send operation pool */
static turbo_pipe_send_op_t* g_pipe_send_op_pool = NULL;
static size_t g_pipe_send_op_pool_size = 0;
static const size_t MAX_PIPE_SEND_OP_POOL_SIZE = 256;

/* Pipe Statistics IDs */
static struct {
    turbo_stat_id_t bytes_sent;
    turbo_stat_id_t bytes_received;
    turbo_stat_id_t send_errors;
    turbo_stat_id_t recv_errors;
    turbo_stat_id_t active_connections;
    turbo_stat_id_t connections_closed;
    int initialized;
} s_pipe_stats = {0};

static void init_pipe_stats(void) {
    if (s_pipe_stats.initialized) return;
    
    s_pipe_stats.bytes_sent = turbo_stats_register("pipe.bytes_sent", TURBO_STAT_COUNTER);
    s_pipe_stats.bytes_received = turbo_stats_register("pipe.bytes_received", TURBO_STAT_COUNTER);
    s_pipe_stats.send_errors = turbo_stats_register("pipe.send_errors", TURBO_STAT_COUNTER);
    s_pipe_stats.recv_errors = turbo_stats_register("pipe.recv_errors", TURBO_STAT_COUNTER);
    s_pipe_stats.active_connections = turbo_stats_register("pipe.active_connections", TURBO_STAT_GAUGE);
    s_pipe_stats.connections_closed = turbo_stats_register("pipe.connections_closed", TURBO_STAT_COUNTER);
    
    s_pipe_stats.initialized = 1;
}

/* Get send operation from pool (thread-safe) */
static turbo_pipe_send_op_t* get_pipe_send_op(turbo_pipe_client_t* client) {
    turbo_pipe_send_op_t* op = NULL;
    
    turbo_pipe_sync_lock();
    if (g_pipe_send_op_pool && g_pipe_send_op_pool_size > 0) {
        op = g_pipe_send_op_pool;
        g_pipe_send_op_pool = op->next;
        g_pipe_send_op_pool_size--;
    }
    turbo_pipe_sync_unlock();
    
    if (!op) {
        op = (turbo_pipe_send_op_t*)malloc(sizeof(turbo_pipe_send_op_t));
        if (op) {
        } else {
            return NULL; // NULL check
        }
    }
    
    memset(op, 0, sizeof(*op));
    op->client = client;
    
    return op;
}

/* Return send operation to pool (thread-safe) */
static void return_pipe_send_op(turbo_pipe_send_op_t* op) {
    if (!op) return;
    
    /* Release all slices */
    if (op->slices) {
        for (size_t i = 0; i < op->slice_count; i++) {
            turbo_arena_slice_release(&op->slices[i]);
        }
        free(op->slices);
        op->slices = NULL;
    }
    
    turbo_pipe_sync_lock();
    if (g_pipe_send_op_pool_size < MAX_PIPE_SEND_OP_POOL_SIZE) {
        op->next = g_pipe_send_op_pool;
        g_pipe_send_op_pool = op;
        g_pipe_send_op_pool_size++;
        turbo_pipe_sync_unlock();
    } else {
        turbo_pipe_sync_unlock();
        free(op);
    }
}

/* Write completion callback */
static void on_pipe_write_complete(uv_write_t* req, int status) {
    turbo_pipe_send_op_t* op = (turbo_pipe_send_op_t*)((char*)req - offsetof(turbo_pipe_send_op_t, req));
    turbo_pipe_client_t* client = op->client;
    
    /* Safety check - don't access client if it's closing */
    if (!client || client->closing) {
        return_pipe_send_op(op);
        return;
    }
    
    client->write_in_progress = 0;
    
    if (status == 0) {
        size_t total_bytes = 0;
        for (size_t i = 0; i < op->slice_count; i++) {
            total_bytes += op->slices[i].length;
        }
        
        turbo_stats_counter_add_fast(s_pipe_stats.bytes_sent, total_bytes);
    } else {
        turbo_stats_counter_inc_fast(s_pipe_stats.send_errors);
    }
    
    return_pipe_send_op(op);
    
    /* Continue sending if more data queued */
    if (client->send_queue_head && !client->closing) {
        turbo_pipe_flush(client);
    }
}

/* Receive buffer allocation */
static void alloc_pipe_recv_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)suggested_size;
    
    turbo_pipe_client_t* client = (turbo_pipe_client_t*)handle->data;
    if (!client || client->closing) {
        buf->base = NULL;
        buf->len = 0;
        return;
    }
    
    /* Use ping-pong buffers for zero-copy receives */
    if (client->recv_buffer2 && client->recv_toggle == 0) {
        buf->base = client->recv_buffer2->data;
        buf->len = (unsigned int)client->recv_buffer2->capacity;
        client->recv_toggle = 1;
    } else if (client->recv_buffer1) {
        buf->base = client->recv_buffer1->data;
        buf->len = (unsigned int)client->recv_buffer1->capacity;
        client->recv_toggle = 0;
    } else {
        buf->base = NULL;
        buf->len = 0;
    }
}

/* Receive callback */
static void on_pipe_recv(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    turbo_pipe_client_t* client = (turbo_pipe_client_t*)stream->data;
    if (!client || client->closing) return;
    
    if (nread < 0) {
        /* EOF or error */
        turbo_stats_counter_inc_fast(s_pipe_stats.recv_errors);
        turbo_pipe_client_close(client);
        return;
    }
    
    if (nread == 0 || !buf || !buf->base) return;
    
    turbo_stats_counter_add_fast(s_pipe_stats.bytes_received, (size_t)nread);
    
    /* Determine which buffer was used */
    turbo_arena_buffer_t* used_buffer = NULL;
    if (client->recv_buffer1 && buf->base == client->recv_buffer1->data) {
        used_buffer = client->recv_buffer1;
    } else if (client->recv_buffer2 && buf->base == client->recv_buffer2->data) {
        used_buffer = client->recv_buffer2;
    }
    
    if (used_buffer && client->on_recv) {
        /* Set the used size in the buffer */
        turbo_arena_buffer_set_used(used_buffer, (size_t)nread);
        
        /* Create zero-copy slice for the received data */
        turbo_arena_slice_t slice = turbo_arena_buffer_slice(used_buffer, 0, (size_t)nread);
        
        /* Call user callback with zero-copy slice */
        int should_close = client->on_recv(client, &slice, NULL);
        
        /* Release the slice (user should have ref'd it if needed) */
        turbo_arena_slice_release(&slice);
        
        if (should_close) {
            turbo_pipe_client_close(client);
        }
    }
}

/* Handle close callback */
static void on_pipe_handle_closed(uv_handle_t* handle) {
    turbo_pipe_client_t* client = (turbo_pipe_client_t*)handle->data;
    if (!client) return;
    
    /* Update connection count */
    if (client->server) {
        client->server->active_connections--;
        turbo_stats_gauge_set_fast(s_pipe_stats.active_connections, client->server->active_connections);
        turbo_stats_counter_inc_fast(s_pipe_stats.connections_closed);
        
        if (client->server->on_close) {
            client->server->on_close(client);
        }
    } else if (client->on_close) {
        client->on_close(client);
    }
    
    /* Cleanup client resources */
    if (client->recv_buffer1) {
        turbo_arena_buffer_unref(client->recv_buffer1);
    }
    if (client->recv_buffer2) {
        turbo_arena_buffer_unref(client->recv_buffer2);
    }
    if (client->write_iov) {
        free(client->write_iov);
    }
    
    /* Free send queue */
    turbo_arena_buffer_t* current = client->send_queue_head;
    while (current) {
        turbo_arena_buffer_t* next = current->next;
        turbo_arena_buffer_unref(current);
        current = next;
    }
    
    turbo_arena_free(&client->arena);
    free(client);
}

/* New connection callback */
static void on_pipe_new_connection(uv_stream_t* server_stream, int status) {
    if (status < 0) {
        turbo_stats_counter_inc_fast(s_pipe_stats.recv_errors);
        return;
    }
    
    turbo_pipe_server_t* server = (turbo_pipe_server_t*)server_stream->data;
    if (!server) return;
    
    /* Create new client */
    turbo_pipe_client_t* client = (turbo_pipe_client_t*)calloc(1, sizeof(*client));
    if (!client) return;
    
    client->server = server;
    client->on_recv = server->on_recv;
    client->on_connect = server->on_connect;
    client->on_close = server->on_close;
    
    /* Initialize client arena */
    if (turbo_arena_init(&client->arena, 0) != 0) {
        free(client);
        return;
    }
    
    /* Initialize pipe handle */
    if (uv_pipe_init(server->loop, &client->handle, 0) != 0) {
        turbo_arena_free(&client->arena);
        free(client);
        return;
    }
    
    client->handle.data = client;
    
    /* Accept the connection */
    if (uv_accept(server_stream, (uv_stream_t*)&client->handle) != 0) {
        uv_close((uv_handle_t*)&client->handle, on_pipe_handle_closed);
        return;
    }
    
    /* Setup receive buffers */
    size_t recv_buf_size = turbo_pipe_config_get_read_buf_size();
    client->recv_buffer1 = turbo_arena_get_buffer(&client->arena, recv_buf_size);
    client->recv_buffer2 = turbo_arena_get_buffer(&client->arena, recv_buf_size);
    
    if (!client->recv_buffer1) {
        uv_close((uv_handle_t*)&client->handle, on_pipe_handle_closed);
        return;
    }
    
    /* Setup write IOV array */
    client->write_iov_capacity = turbo_pipe_config_get_max_write_iov();
    client->write_iov = (uv_buf_t*)malloc(client->write_iov_capacity * sizeof(uv_buf_t));
    if (!client->write_iov) {
        turbo_pipe_client_close(client);
        return;
    }
    
    /* Start reading */
    if (uv_read_start((uv_stream_t*)&client->handle, alloc_pipe_recv_buffer, on_pipe_recv) == 0) {
        server->active_connections++;
        turbo_stats_gauge_set_fast(s_pipe_stats.active_connections, server->active_connections);

        if (client->on_connect) {
            /* turbo_connect_cb signature: (void* handle, int status, void* peer)
             * For server accept: handle=client, status=0, peer=server */
            client->on_connect(client, 0, server);
        }
    } else {
        uv_close((uv_handle_t*)&client->handle, on_pipe_handle_closed);
    }
}/* 
Server initialization */
int turbo_pipe_server_init(turbo_pipe_server_t* server, uv_loop_t* loop, const char* name) {
    if (!server || !loop || !name) return UV_EINVAL;
    
    memset(server, 0, sizeof(*server));
    server->loop = loop;
    
    init_pipe_stats();
    
    /* Initialize server arena */
    if (turbo_arena_init(&server->arena, 0) != 0) {
        return UV_ENOMEM;
    }
    
    /* Create pipe handle */
    server->handle = (uv_pipe_t*)malloc(sizeof(uv_pipe_t));
    if (!server->handle) {
        turbo_arena_free(&server->arena);
        return UV_ENOMEM;
    }
    
    int rc = uv_pipe_init(loop, server->handle, 0);
    if (rc != 0) {
        free(server->handle);
        turbo_arena_free(&server->arena);
        return rc;
    }
    
    server->handle->data = server;
    
    /* Bind to named pipe */
    rc = uv_pipe_bind(server->handle, name);
    if (rc != 0) {
        uv_close((uv_handle_t*)server->handle, NULL);
        turbo_arena_free(&server->arena);
        return rc;
    }
    

    return 0;
}

/* Start server */
int turbo_pipe_server_start(turbo_pipe_server_t* server,
                          turbo_recv_cb on_recv,
                          turbo_connect_cb on_connect,
                          turbo_close_cb on_close) {
    if (!server || !server->handle) return UV_EINVAL;
    
    server->on_recv = on_recv;
    server->on_connect = on_connect;
    server->on_close = on_close;
    
    int backlog = turbo_pipe_config_get_backlog();
    return uv_listen((uv_stream_t*)server->handle, backlog, on_pipe_new_connection);
}

/* Close callback for server handle */
static void on_pipe_server_close(uv_handle_t* handle) {
    free(handle);
}

/* Stop server */
void turbo_pipe_server_stop(turbo_pipe_server_t* server) {
    if (!server) return;

    if (server->handle) {
        if (!uv_is_closing((uv_handle_t*)server->handle)) {
            uv_close((uv_handle_t*)server->handle, on_pipe_server_close);
        }
        server->handle = NULL;
    }

    turbo_arena_free(&server->arena);
}

/* Create client */
turbo_pipe_client_t* turbo_pipe_client_create(uv_loop_t* loop) {
    if (!loop) return NULL;
    
    turbo_pipe_client_t* client = (turbo_pipe_client_t*)calloc(1, sizeof(*client));
    if (!client) return NULL;
    
    client->is_client_mode = 1;
    
    init_pipe_stats();
    
    /* Initialize client arena */
    if (turbo_arena_init(&client->arena, 0) != 0) {
        free(client);
        return NULL;
    }
    
    /* Initialize pipe handle */
    if (uv_pipe_init(loop, &client->handle, 0) != 0) {
        turbo_arena_free(&client->arena);
        free(client);
        return NULL;
    }
    
    client->handle.data = client;
    
    /* Setup receive buffers */
    size_t recv_buf_size = turbo_pipe_config_get_read_buf_size();
    client->recv_buffer1 = turbo_arena_get_buffer(&client->arena, recv_buf_size);
    client->recv_buffer2 = turbo_arena_get_buffer(&client->arena, recv_buf_size);
    
    /* Setup write IOV array */
    client->write_iov_capacity = turbo_pipe_config_get_max_write_iov();
    client->write_iov = (uv_buf_t*)malloc(client->write_iov_capacity * sizeof(uv_buf_t));
    if (!client->write_iov) {
        turbo_arena_free(&client->arena);
        free(client);
        return NULL;
    }
    
    return client;
}

/* Client connect callback */
static void on_pipe_client_connected(uv_connect_t* req, int status) {
    turbo_pipe_client_t* client = (turbo_pipe_client_t*)req->handle->data;
    if (!client) {
        free(req);
        return;
    }
    
    if (status == 0) {
        /* Start reading */
        uv_read_start((uv_stream_t*)&client->handle, alloc_pipe_recv_buffer, on_pipe_recv);

        if (client->on_connect) {
            client->on_connect(client, 0, NULL);
        }
    } else {
        turbo_stats_counter_inc_fast(s_pipe_stats.recv_errors);
        if (status == UV_ETIMEDOUT || status == UV_ECONNREFUSED || status == UV_ECONNRESET || status == UV_ENOENT) {
            TLOG_DEBUG("Pipe connection failed: {:s}", uv_strerror(status));
        } else {
            TLOG_ERROR("Pipe connection failed: {:s}", uv_strerror(status));
        }
        
        // Notify caller of connection failure
        if (client->on_connect) {
            client->on_connect(client, status, NULL);
        }
        
        // Clean up internally - user doesn't know client structs
       // turbo_pipe_client_close(client);
    }
    
    free(req);
}

/* Connect client */
int turbo_pipe_client_connect(turbo_pipe_client_t* client,
                            const char* name,
                            turbo_recv_cb on_recv,
                            turbo_connect_cb on_connect,
                            turbo_close_cb on_close) {
    if (!client || !name) return UV_EINVAL;
    
    client->on_recv = on_recv;
    client->on_connect = on_connect;
    client->on_close = on_close;
    
    uv_connect_t* connect_req = (uv_connect_t*)malloc(sizeof(uv_connect_t));
    if (!connect_req) return UV_ENOMEM;
    
    uv_pipe_connect(connect_req, &client->handle, name, on_pipe_client_connected);
    return 0;
}

/* Start reading on client (re-arms alloc + read callbacks) */
int turbo_pipe_read_start(turbo_pipe_client_t* client) {
    if (!client || client->closing) return UV_EINVAL;
    return uv_read_start((uv_stream_t*)&client->handle, alloc_pipe_recv_buffer, on_pipe_recv);
}

/* Stop reading on client */
void turbo_pipe_read_stop(turbo_pipe_client_t* client) {
    if (!client) return;
    uv_read_stop((uv_stream_t*)&client->handle);
}

/* Close client */
void turbo_pipe_client_close(turbo_pipe_client_t* client) {
    if (!client || client->closing) return;
    
    client->closing = 1;
    uv_read_stop((uv_stream_t*)&client->handle);
    
    if (!uv_is_closing((uv_handle_t*)&client->handle)) {
        uv_close((uv_handle_t*)&client->handle, on_pipe_handle_closed);
    }
}

/* Get zero-copy send buffer */
turbo_arena_buffer_t* turbo_pipe_get_send_buffer(turbo_pipe_client_t* client, size_t min_size) {
    if (!client) return NULL;
    
    return turbo_arena_get_buffer(&client->arena, min_size);
}

/* Send buffer with zero-copy */
int turbo_pipe_send_buffer(turbo_pipe_client_t* client, turbo_arena_buffer_t* buffer, size_t length) {
    if (!client || !buffer || client->closing) return UV_EINVAL;
    
    if (length > buffer->used) return UV_EINVAL;
    
    /* Add to send queue */
    buffer->next = NULL;
    if (client->send_queue_tail) {
        client->send_queue_tail->next = buffer;
    } else {
        client->send_queue_head = buffer;
    }
    client->send_queue_tail = buffer;
    client->send_queue_bytes += length;
    
    /* Reference the buffer */
    turbo_arena_buffer_ref(buffer);
    
    /* Flush if not already writing */
    if (!client->write_in_progress) {
        return turbo_pipe_flush(client);
    }
    
    return 0;
}

/* Flush pending writes */
int turbo_pipe_flush(turbo_pipe_client_t* client) {
    if (!client || client->closing || client->write_in_progress) return UV_EINVAL;
    if (!client->send_queue_head) return 0;
    
    turbo_pipe_send_op_t* op = get_pipe_send_op(client);
    if (!op) return UV_ENOMEM;
    
    /* Count buffers in queue */
    size_t buffer_count = 0;
    turbo_arena_buffer_t* current = client->send_queue_head;
    while (current && buffer_count < client->write_iov_capacity) {
        buffer_count++;
        current = current->next;
    }
    
    /* Create slices and IOV array */
    op->slices = (turbo_arena_slice_t*)malloc(buffer_count * sizeof(turbo_arena_slice_t));
    if (!op->slices) {
        return_pipe_send_op(op);
        return UV_ENOMEM;
    }
    
    op->slice_count = buffer_count;
    current = client->send_queue_head;
    
    for (size_t i = 0; i < buffer_count; i++) {
        op->slices[i] = turbo_arena_buffer_slice(current, 0, current->used);
        client->write_iov[i] = uv_buf_init(op->slices[i].data, (unsigned int)op->slices[i].length);
        
        turbo_arena_buffer_t* next = current->next;
        turbo_arena_buffer_unref(current);
        current = next;
    }
    
    /* Update queue */
    client->send_queue_head = current;
    if (!current) {
        client->send_queue_tail = NULL;
        client->send_queue_bytes = 0;
    }
    
    /* Send the data */
    client->write_in_progress = 1;
    int rc = uv_write(&op->req, (uv_stream_t*)&client->handle, client->write_iov, 
                      (unsigned int)buffer_count, on_pipe_write_complete);
    
    if (rc != 0) {
        client->write_in_progress = 0;
        return_pipe_send_op(op);
        return rc;
    }
    

    return 0;
}

/* Fallback copy-based send */
int turbo_pipe_send(turbo_pipe_client_t* client, const char* data, size_t length) {
    if (!client || !data || length == 0) return UV_EINVAL;
    
    turbo_arena_buffer_t* buffer = turbo_pipe_get_send_buffer(client, length);
    if (!buffer) return UV_ENOMEM;
    
    memcpy(buffer->data, data, length);
    turbo_arena_buffer_set_used(buffer, length);
    
    int rc = turbo_pipe_send_buffer(client, buffer, length);
    turbo_arena_buffer_unref(buffer);

    return rc;
}

/* Discard buffer */
void turbo_pipe_discard_buffer(turbo_pipe_client_t* client, turbo_arena_buffer_t* buffer) {
    if (!client || !buffer) return;
    turbo_arena_buffer_unref(buffer);
}

/* Scatter-gather send (ZERO-COPY with external buffer wrapping) */
int turbo_pipe_sendv(turbo_pipe_client_t* client, const turbo_pipe_iovec_t* iov, size_t iovcnt) {
    if (!client || !iov || iovcnt == 0) return UV_EINVAL;
    
    int rc = 0;
    
    /* Queue all buffers using zero-copy wrappers (NO MEMCPY!) */
    for (size_t i = 0; i < iovcnt && rc == 0; i++) {
        if (iov[i].len > 0 && iov[i].data) {
            /* Wrap user buffer - ZERO COPY! */
            turbo_arena_buffer_t* buffer = turbo_arena_wrap_external(
                (void*)iov[i].data,
                iov[i].len,
                NULL,  /* No free callback - user manages memory */
                NULL
            );
            if (!buffer) {
                rc = UV_ENOMEM;
                break;
            }
            
            rc = turbo_pipe_send_buffer(client, buffer, iov[i].len);
            turbo_arena_buffer_unref(buffer);
        }
    }
    
    /* Flush all queued buffers atomically */
    if (rc == 0) {
        rc = turbo_pipe_flush(client);
    }

    return rc;
}

/* Get statistics */
void turbo_pipe_get_stats(const turbo_pipe_server_t* server, turbo_pipe_stats_t* stats) {
    if (!server || !stats) return;
    
    memset(stats, 0, sizeof(*stats));
    
    /* Get arena statistics */
    turbo_arena_get_stats(&server->arena, &stats->arena_stats);
    
    /* Get global pipe statistics from stats system */
    turbo_stat_entry_t* entry;
    
    entry = turbo_stats_get("pipe.bytes_sent");
    stats->bytes_sent = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.bytes_received");
    stats->bytes_received = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.messages_sent");
    stats->messages_sent = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.messages_received");
    stats->messages_received = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.zero_copy_sends");
    stats->zero_copy_sends = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.copy_sends");
    stats->copy_sends = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.zero_copy_receives");
    stats->zero_copy_receives = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.active_connections");
    stats->active_connections = entry ? entry->data.gauge : 0;
    
    entry = turbo_stats_get("pipe.connections_established");
    stats->connections_established = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.connections_closed");
    stats->connections_closed = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.send_errors");
    stats->send_errors = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.recv_errors");
    stats->recv_errors = entry ? entry->data.counter : 0;
    
    entry = turbo_stats_get("pipe.client_connection_errors");
    stats->connection_errors = entry ? entry->data.counter : 0;
}

/* Reset statistics */
void turbo_pipe_reset_stats(turbo_pipe_server_t* server) {
    if (!server) return;
    
    turbo_stats_reset("pipe.bytes_sent");
    turbo_stats_reset("pipe.bytes_received");
    turbo_stats_reset("pipe.messages_sent");
    turbo_stats_reset("pipe.messages_received");
    turbo_stats_reset("pipe.zero_copy_sends");
    turbo_stats_reset("pipe.copy_sends");
    turbo_stats_reset("pipe.zero_copy_receives");
    turbo_stats_reset("pipe.connections_established");
    turbo_stats_reset("pipe.connections_closed");
    turbo_stats_reset("pipe.send_errors");
    turbo_stats_reset("pipe.recv_errors");
    turbo_stats_reset("pipe.client_connection_errors");
}

/* Trim memory */
void turbo_pipe_trim_memory(turbo_pipe_server_t* server) {
    if (!server) return;
    turbo_arena_trim(&server->arena);
}

/* Get memory usage */
size_t turbo_pipe_get_memory_usage(const turbo_pipe_server_t* server) {
    if (!server) return 0;
    
    turbo_arena_stats_t stats;
    turbo_arena_get_stats(&server->arena, &stats);
    return stats.total_allocated;
}

/* Cleanup global pools */
void turbo_pipe_cleanup_pools(void) {
    while (g_pipe_send_op_pool) {
        turbo_pipe_send_op_t* op = g_pipe_send_op_pool;
        g_pipe_send_op_pool = op->next;
        free(op);
    }
    g_pipe_send_op_pool_size = 0;
    
    g_pipe_send_op_pool_size = 0;
}
