#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "turbo_tcp.h"
#include "turbo_coro_context.h"
#include "client_common.h"
#include "turbo_dns.h"
#include "internal.h"
#include "tlog.h"
#include "disruptor.h"

#ifdef _WIN32
#include <winsock2.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

/* Enhanced TCP with True Zero-Copy Implementation */

/* Forward declarations for global synchronization */
extern void turbo_tcp_sync_lock(void);
extern void turbo_tcp_sync_unlock(void);

/* Send operation for zero-copy */
typedef struct turbo_tcp_send_op_s {
    uv_write_t req;
    mem_slice_t* slices;
    size_t slice_count;
    turbo_tcp_client_t* client;
    struct turbo_tcp_send_op_s* next;
} turbo_tcp_send_op_t;

/* ── Send op pool: simple free-list ────────────────────────── */
#define TCP_SEND_OP_POOL_CAPACITY 256

/* Pre-allocated send op slab */
static turbo_tcp_send_op_t g_tcp_send_op_slab[TCP_SEND_OP_POOL_CAPACITY];

/* Free-list head (LIFO stack via the `next` pointer in each op) */
static turbo_tcp_send_op_t *g_tcp_send_op_free = NULL;
static int                  g_tcp_send_op_init = 0;

static void ensure_tcp_send_op_pool(void) {
    if (g_tcp_send_op_init) return;
    for (int i = 0; i < TCP_SEND_OP_POOL_CAPACITY; i++) {
        g_tcp_send_op_slab[i].next = g_tcp_send_op_free;
        g_tcp_send_op_free = &g_tcp_send_op_slab[i];
    }
    g_tcp_send_op_init = 1;
}

/* Get send operation from pool, fallback to malloc */
static turbo_tcp_send_op_t* get_tcp_send_op(turbo_tcp_client_t* client) {
    ensure_tcp_send_op_pool();

    turbo_tcp_send_op_t* op = g_tcp_send_op_free;
    if (op) {
        g_tcp_send_op_free = op->next;
    } else {
        op = (turbo_tcp_send_op_t*)malloc(sizeof(turbo_tcp_send_op_t));
    }

    if (op) {
        memset(op, 0, sizeof(*op));
        op->client = client;
    }
    return op;
}

/* Return send operation to pool */
static void return_tcp_send_op(turbo_tcp_send_op_t* op) {
    if (!op) return;

    /* Release all slices */
    if (op->slices) {
        for (size_t i = 0; i < op->slice_count; i++) {
            mem_slice_release(&op->slices[i]);
        }
        free(op->slices);
        op->slices = NULL;
    }

    /* Return to slab if it belongs there, else free */
    if (op >= &g_tcp_send_op_slab[0] &&
        op <  &g_tcp_send_op_slab[TCP_SEND_OP_POOL_CAPACITY]) {
        op->next = g_tcp_send_op_free;
        g_tcp_send_op_free = op;
    } else {
        free(op);
    }
}

/* Write completion callback */
static void on_tcp_write_complete(uv_write_t* req, int status) {
    turbo_tcp_send_op_t* op = (turbo_tcp_send_op_t*)((char*)req - offsetof(turbo_tcp_send_op_t, req));
    turbo_tcp_client_t* client = op->client;
    
    client->write_in_progress = 0;
    
    return_tcp_send_op(op);
    
    if (client->on_write_complete) {
        client->on_write_complete(client, status);
    }
    
    /* Continue sending if more data queued */
    if (client->send_queue_head && !client->closing) {
        turbo_tcp_flush(client);
    }
}

/* Receive buffer allocation */
static void alloc_tcp_recv_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    (void)suggested_size;
    
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)handle->data;
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
static void on_tcp_recv(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)stream->data;
    if (!client || client->closing) return;

    if (nread < 0) {
        /* EOF or error - notify callback. Upper layer decides when to close. */
        if (client->on_recv) {
            client->on_recv(client, NULL, NULL);
        }
        return;
    }
    
    if (nread == 0 || !buf || !buf->base) return;
    

    
    /* Determine which buffer was used */
    mem_buffer_t* used_buffer = NULL;
    if (client->recv_buffer1 && buf->base == client->recv_buffer1->data) {
        used_buffer = client->recv_buffer1;
    } else if (client->recv_buffer2 && buf->base == client->recv_buffer2->data) {
        used_buffer = client->recv_buffer2;
    }
    
    if (used_buffer && client->on_recv) {
        /* Set the used size in the buffer */
        mem_set_used(used_buffer, (size_t)nread);
        
        /* Create zero-copy slice for the received data */
        mem_slice_t slice = mem_slice(used_buffer, 0, (size_t)nread);
        
        /* Call user callback with zero-copy slice */
        int should_close = client->on_recv(client, &slice, NULL);
        
        /* Release the slice (user should have ref'd it if needed) */
        mem_slice_release(&slice);
        
        if (should_close) {
            turbo_tcp_client_close(client);
        }
    }
}

/* Handle close callback */
static void on_tcp_handle_closed(uv_handle_t* handle) {
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)handle->data;
    if (!client) return;
    
    /* Cleanup client resources first */
    if (client->recv_buffer1) {
        mem_unref(client->recv_buffer1);
    }
    if (client->recv_buffer2) {
        mem_unref(client->recv_buffer2);
    }
    if (client->write_iov) {
        free(client->write_iov);
    }
    
    /* Free send queue */
    mem_buffer_t* current = client->send_queue_head;
    while (current) {
        mem_buffer_t* next = current->next;
        mem_unref(current);
        current = next;
    }
    
    
    
    if (client->dns_initialized) {
        turbo_dns_cleanup();
        client->dns_initialized = 0;
    }

    /* Finally, trigger callbacks and structural cleanup */
    if (client->server) {
        client->server->active_connections--;
        
        TLOG_DEBUG("TCP connection closed (server mode), active connections: {:d}", 
                   client->server->active_connections);

        if (client->server->on_close) {
            client->server->on_close(client);
        }
    } else if (client->on_close) {
        client->on_close(client);
    }
    
    if (!client->managed) {
        free(client);
    }
}

/* New connection callback */
static void on_tcp_new_connection(uv_stream_t* server_stream, int status) {
    if (status < 0) {
        return;
    }
    
    turbo_tcp_server_t* server = (turbo_tcp_server_t*)server_stream->data;
    if (!server) return;
    
    /* Create new client */
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)calloc(1, sizeof(*client));
    if (!client) return;
    
    client->server = server;
    client->on_recv = server->on_recv;
    client->on_connect = server->on_connect;
    client->on_close = server->on_close;
    
    /* Initialize client arena */
    client->arena = (mem_pool_t*)coro_get_memory_pool();
    /* Initialize TCP handle */
    if (uv_tcp_init(server->loop, &client->handle) != 0) {
        
        free(client);
        return;
    }
    
    client->handle.data = client;
    
    /* Accept the connection */
    if (uv_accept(server_stream, (uv_stream_t*)&client->handle) != 0) {
        uv_close((uv_handle_t*)&client->handle, on_tcp_handle_closed);
        return;
    }
    
    /* Setup receive buffers */
    client->recv_buffer1 = mem_get_buffer(client->arena, 8192);
    client->recv_buffer2 = mem_get_buffer(client->arena, 8192);
    
    if (!client->recv_buffer1) {
        uv_close((uv_handle_t*)&client->handle, on_tcp_handle_closed);
        return;
    }
    
    /* Setup write IOV array */
    client->write_iov_capacity = 64;
    client->write_iov = (uv_buf_t*)malloc(client->write_iov_capacity * sizeof(uv_buf_t));
    if (!client->write_iov) {
        turbo_tcp_client_close(client);
        return;
    }
    
    /* Enable TCP_NODELAY */
    uv_tcp_nodelay(&client->handle, 1);
    
    /* Start reading */
    if (uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_recv_buffer, on_tcp_recv) == 0) {
        server->active_connections++;
        
        TLOG_DEBUG("TCP connection accepted, active connections: {:d}", server->active_connections);

        if (client->on_connect) {
            client->on_connect(client, 0, server);
        }
    } else {
        uv_close((uv_handle_t*)&client->handle, on_tcp_handle_closed);
    }
}
/*Server initialization */
int turbo_tcp_server_init(turbo_tcp_server_t* server, uv_loop_t* loop,
                           const char* host, unsigned short port) {
    if (!server || !loop) return UV_EINVAL;
    
    memset(server, 0, sizeof(*server));
    server->loop = loop;
    
    /* Initialize server arena */
    
    
    /* Create TCP handle */
    server->handle = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    if (!server->handle) {
        
        return UV_ENOMEM;
    }
    
    int rc = uv_tcp_init(loop, server->handle);
    if (rc != 0) {
        free(server->handle);
        
        return rc;
    }
    
    server->handle->data = server;
    
    /* Bind to address */
    struct sockaddr_in addr;
    const char* bind_host = host ? host : "0.0.0.0";
    rc = uv_ip4_addr(bind_host, (int)port, &addr);
    if (rc != 0) {
        uv_close((uv_handle_t*)server->handle, NULL);
        
        return rc;
    }
    
    rc = uv_tcp_bind(server->handle, (const struct sockaddr*)&addr, 0);
    if (rc != 0) {
        uv_close((uv_handle_t*)server->handle, NULL);
        
        return rc;
    }
    
    TLOG_INFO("TCP server initialized on {:s}:{:d}", bind_host, port);
    return 0;
}

/* Start server */
int turbo_tcp_server_start(turbo_tcp_server_t* server,
                                  turbo_recv_cb on_recv,
                                  turbo_connect_cb on_connect,
                                  turbo_close_cb on_close) {
    if (!server || !server->handle) return UV_EINVAL;
    
    server->on_recv = on_recv;
    server->on_connect = on_connect;
    server->on_close = on_close;
    
    int rc = uv_listen((uv_stream_t*)server->handle, 128, on_tcp_new_connection);
    if (rc == 0) {
        TLOG_INFO("TCP server listening");
    } else {
        TLOG_ERROR("TCP server listen failed: {:s}", uv_strerror(rc));
    }
    return rc;
}

/* Close callback for server handle */
static void on_tcp_server_close(uv_handle_t* handle) {
    free(handle);
}

/* Stop server */
void turbo_tcp_server_stop(turbo_tcp_server_t* server) {
    if (!server) return;

    if (server->handle) {
        if (!uv_is_closing((uv_handle_t*)server->handle)) {
            uv_close((uv_handle_t*)server->handle, on_tcp_server_close);
        }
        server->handle = NULL;
    }

    


}

/* Create client */
turbo_tcp_client_t* turbo_tcp_client_create(uv_loop_t* loop) {
    if (!loop) return NULL;
    
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)calloc(1, sizeof(*client));
    if (!client) return NULL;
    
    client->is_client_mode = 1;
    
    /* Initialize client arena */
    client->arena = (mem_pool_t*)coro_get_memory_pool();
    
    /* Initialize TCP handle */
    if (uv_tcp_init(loop, &client->handle) != 0) {
        
        free(client);
        return NULL;
    }
    
    client->handle.data = client;
    
    /* Setup receive buffers */
    client->recv_buffer1 = mem_get_buffer(client->arena, 8192);
    client->recv_buffer2 = mem_get_buffer(client->arena, 8192);
    
    /* Setup write IOV array */
    client->write_iov_capacity = 64;
    client->write_iov = (uv_buf_t*)malloc(client->write_iov_capacity * sizeof(uv_buf_t));
    if (!client->write_iov) {
        
        free(client);
        return NULL;
    }
    
    return client;
}


/* Forward declaration */
static void on_tcp_client_connected(uv_connect_t* req, int status);

/* Connect context */
typedef struct {
    turbo_tcp_client_t* client;
    unsigned short port;
} turbo_connect_ctx_t;

/* DNS resolution callback */
static void on_connect_dns_resolved(const char* hostname, const char* ip, int status, void* user_data) {
    turbo_connect_ctx_t* ctx = (turbo_connect_ctx_t*)user_data;
    turbo_tcp_client_t* client = ctx->client;
    
    /* Guard against duplicate callbacks or invalid state */
    if (client->conn_state != 1) {
        TLOG_DEBUG("on_connect_dns_resolved: ignoring callback in state {:d}", client->conn_state);
        free(ctx);
        return;
    }

    if (status != 0 || !ip) {
        client->conn_state = 0;
        if (client->on_connect) {
            client->on_connect(client, status ? status : UV_EAI_FAIL, NULL);
        }
        free(ctx);
        return;
    }

    struct sockaddr_storage addr;
    if (turbo_dns_parse_address(ip, (int)ctx->port, &addr) != 0) {
        client->conn_state = 0;
        if (client->on_connect) {
            client->on_connect(client, UV_EAI_FAIL, NULL);
        }
        free(ctx);
        return;
    }

    /* We have an address, now connect */
    uv_connect_t* connect_req = (uv_connect_t*)malloc(sizeof(uv_connect_t));
    if (!connect_req) {
        client->conn_state = 0;
        if (client->on_connect) {
            client->on_connect(client, UV_ENOMEM, NULL);
        }
        free(ctx);
        return;
    }

    /* Store client in request handle data (uv_tcp_connect uses client->handle) */
    /* Wait, uv_tcp_connect takes handle. req->handle will point to client->handle after connect starts */

    client->conn_state = 2; /* Connecting */
    int rc = uv_tcp_connect(connect_req, &client->handle, (const struct sockaddr*)&addr, on_tcp_client_connected);
    if (rc != 0) {
        client->conn_state = 0;
        free(connect_req);
        if (client->on_connect) {
            client->on_connect(client, rc, NULL);
        }
    }
    
    free(ctx);
}

/* Client connect callback */
static void on_tcp_client_connected(uv_connect_t* req, int status) {
    turbo_tcp_client_t* client = (turbo_tcp_client_t*)req->handle->data;
    if (!client) return;
    
    // Cleanup connect req
    free(req);

    if (status == 0) {
        client->conn_state = 3; /* Connected */
        /* Enable TCP_NODELAY */
        uv_tcp_nodelay(&client->handle, 1);

        /* Start reading — needed by upper layers (WS/TLS handshake).
           Coro transport callbacks handle the case where data arrives
           before a coroutine is waiting. */
        uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_recv_buffer, on_tcp_recv);

        TLOG_DEBUG("TCP client connected successfully");
        if (client->on_connect) {
            client->on_connect(client, 0, NULL);
        }
    } else {
        client->conn_state = 0;
        if (status == UV_ETIMEDOUT || status == UV_ECONNREFUSED || status == UV_ECONNRESET) {
            TLOG_DEBUG("TCP connection failed: {:s}", uv_strerror(status));
        } else {
            TLOG_ERROR("TCP connection failed: {:s}", uv_strerror(status));
        }
        /* Notify user of failure (on_connect) implies we don't close? 
           But turbo_tcp_client_connect usually expects on_connect called.
           Actually the pattern used here is to call on_connect with error? 
           Wait, existing code called turbo_tcp_client_close(client). */
        
        /* Check if on_connect has been called? 
           If status != 0, we received an error callback. We should notify user. */
        if (client->on_connect) {
            client->on_connect(client, status, NULL);
        }
        
        /* Don't close immediately if we want to allow retry? 
           But turbo_tcp_client_close was called in original code. */
        turbo_tcp_client_close(client);
    }
}

/* Connect client */
int turbo_tcp_client_connect(turbo_tcp_client_t* client,
                                    const char* host, unsigned short port,
                                    turbo_recv_cb on_recv,
                                    turbo_connect_cb on_connect,
                                    turbo_close_cb on_close) {
    if (!client || !host) return UV_EINVAL;
    
    if (client->conn_state != 0) {
        return UV_EALREADY;
    }

    client->on_recv = on_recv;
    client->on_connect = on_connect;
    client->on_close = on_close;
    
    turbo_connect_ctx_t* ctx = (turbo_connect_ctx_t*)malloc(sizeof(turbo_connect_ctx_t));
    if (!ctx) return UV_ENOMEM;
    
    ctx->client = client;
    ctx->port = port;

    /* Initialize DNS subsystem */
    int rc = turbo_dns_init();
    if (rc != 0) {
        free(ctx);
        return rc;
    }
    client->dns_initialized = 1;

    /* Use async DNS resolution to avoid blocking the event loop */
    client->conn_state = 1; /* Resolving */
    rc = turbo_dns_resolve_async(client->handle.loop, host, TURBO_DNS_ANY, on_connect_dns_resolved, ctx);
    if (rc != 0) {
        client->conn_state = 0;
        free(ctx);
        /* Cleanup DNS if we failed to start resolution */
        /* wait, if we return error, user might call close? 
           If we return error, client is still allocated. user should call close.
           But usually user calls create, then connect. If connect fails:
             client = turbo_tcp_client_create()
             if (turbo_tcp_client_connect() != 0) { turbo_tcp_client_close(client); }
           So cleanup will happen in on_tcp_handle_closed. */
        return rc;
    }
    
    return 0;
}

/* Start reading on client (re-arms alloc + read callbacks) */
int turbo_tcp_read_start(turbo_tcp_client_t* client) {
    if (!client || client->closing) return UV_EINVAL;
    return uv_read_start((uv_stream_t*)&client->handle, alloc_tcp_recv_buffer, on_tcp_recv);
}

/* Stop reading on client */
void turbo_tcp_read_stop(turbo_tcp_client_t* client) {
    if (!client) return;
    uv_read_stop((uv_stream_t*)&client->handle);
}

/* Close client */
void turbo_tcp_client_close(turbo_tcp_client_t* client) {
    if (!client || client->closing) return;
    
    client->conn_state = 0;
    client->closing = 1;
    uv_read_stop((uv_stream_t*)&client->handle);
    
    if (!uv_is_closing((uv_handle_t*)&client->handle)) {
        uv_close((uv_handle_t*)&client->handle, on_tcp_handle_closed);
    } else {
        /* Already closing? We might need to ensure callback is called? 
           uv_close guarantees callback. If already closing, callback is pending. */
    }
}

/* Get zero-copy send buffer */
mem_buffer_t* turbo_tcp_get_send_buffer(turbo_tcp_client_t* client, size_t min_size) {
    if (!client) return NULL;
    
    return mem_get_buffer(client->arena, min_size);
}

/* Queue buffer without auto-flush (for scatter-gather) */
int turbo_tcp_queue_buffer(turbo_tcp_client_t* client, mem_buffer_t* buffer, size_t length) {
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
    mem_ref(buffer);
    
    return 0;
}

/* Send buffer with zero-copy (auto-flush) */
int turbo_tcp_send_buffer(turbo_tcp_client_t* client, mem_buffer_t* buffer, size_t length) {
    if (!client || !buffer || client->closing) return UV_EINVAL;
    
    if (length > buffer->used) return UV_EINVAL;
    
    /* Queue the buffer */
    int rc = turbo_tcp_queue_buffer(client, buffer, length);
    if (rc != 0) return rc;
    
    /* Flush if not already writing */
    if (!client->write_in_progress) {
        return turbo_tcp_flush(client);
    }
    
    return 0;
}

/* Flush pending writes */
int turbo_tcp_flush(turbo_tcp_client_t* client) {
    if (!client || client->closing || client->write_in_progress) return UV_EINVAL;
    if (!client->send_queue_head) return 0;
    
    turbo_tcp_send_op_t* op = get_tcp_send_op(client);
    if (!op) return UV_ENOMEM;
    
    /* Count buffers in queue */
    size_t buffer_count = 0;
    mem_buffer_t* current = client->send_queue_head;
    while (current && buffer_count < client->write_iov_capacity) {
        buffer_count++;
        current = current->next;
    }
    
    /* Create slices and IOV array */
    op->slices = (mem_slice_t*)malloc(buffer_count * sizeof(mem_slice_t));
    if (!op->slices) {
        return_tcp_send_op(op);
        return UV_ENOMEM;
    }
    
    op->slice_count = buffer_count;
    current = client->send_queue_head;
    
    for (size_t i = 0; i < buffer_count; i++) {
        op->slices[i] = mem_slice(current, 0, current->used);
        client->write_iov[i] = uv_buf_init(op->slices[i].data, (unsigned int)op->slices[i].length);
        
        mem_buffer_t* next = current->next;
        mem_unref(current);
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
                      (unsigned int)buffer_count, on_tcp_write_complete);
    
    if (rc != 0) {
        client->write_in_progress = 0;
        return_tcp_send_op(op);
        return rc;
    }
    
    return 0;
}

/* Fallback copy-based send */
int turbo_tcp_send(turbo_tcp_client_t* client, const char* data, size_t length) {
    if (!client || !data || length == 0) return UV_EINVAL;

    mem_buffer_t* buffer = turbo_tcp_get_send_buffer(client, length);
    if (!buffer) return UV_ENOMEM;
    
    memcpy(buffer->data, data, length);
    mem_set_used(buffer, length);
    
    int rc = turbo_tcp_send_buffer(client, buffer, length);
    mem_unref(buffer);

    return rc;
}

/* Discard buffer */
void turbo_tcp_discard_buffer(turbo_tcp_client_t* client, mem_buffer_t* buffer) {
    if (!client || !buffer) return;
    mem_unref(buffer);
}

/* Scatter-gather send (ZERO-COPY with external buffer wrapping) */
int turbo_tcp_sendv(turbo_tcp_client_t* client, const turbo_iovec_t* iov, size_t iovcnt) {
    if (!client || !iov || iovcnt == 0) return UV_EINVAL;
    
    int rc = 0;
    
    /* Queue all buffers using zero-copy wrappers (NO MEMCPY!) */
    for (size_t i = 0; i < iovcnt && rc == 0; i++) {
        if (iov[i].len > 0 && iov[i].data) {
            /* Wrap user buffer - ZERO COPY! */
            mem_buffer_t* buffer = mem_wrap_external(
                (void*)iov[i].data,
                iov[i].len,
                NULL,  /* No free callback - user manages memory */
                NULL
            );
            if (!buffer) {
                rc = UV_ENOMEM;
                break;
            }
            
            rc = turbo_tcp_queue_buffer(client, buffer, iov[i].len);
            mem_unref(buffer);
        }
    }
    
    /* Flush all queued buffers atomically */
    if (rc == 0) {
        rc = turbo_tcp_flush(client);
    }
    
    return rc;
}



/* Trim memory */
void turbo_tcp_trim_memory(turbo_tcp_server_t* server) {
    if (!server) return;
    mem_trim(server->arena);
}


/* Cleanup global pools */
void turbo_tcp_cleanup_pools(void) {
    g_tcp_send_op_free = NULL;
    g_tcp_send_op_init = 0;
}

/* ── SOCKS5 Proxy Support ────────────────────────────────────── */

/* SOCKS5 handshake states */
typedef enum {
    SOCKS5_STATE_METHOD_SEND,
    SOCKS5_STATE_METHOD_RECV,
    SOCKS5_STATE_AUTH_SEND,
    SOCKS5_STATE_AUTH_RECV,
    SOCKS5_STATE_CONNECT_SEND,
    SOCKS5_STATE_CONNECT_RECV,
    SOCKS5_STATE_CONNECTED
} socks5_handshake_state_t;

/* Forward declaration */
typedef struct socks5_handshake_ctx_s socks5_handshake_ctx_t;

/* Context for proxy connection */
typedef struct {
    turbo_tcp_client_t* client;
    char target_host[256];
    uint16_t target_port;
    turbo_socks5_config_t proxy_config;
    turbo_recv_cb on_recv;
    turbo_connect_cb on_connect;
    turbo_close_cb on_close;
    uv_timer_t timeout_timer;
    int timed_out;
    socks5_handshake_ctx_t* handshake_ctx;
} turbo_proxy_connect_ctx_t;

/* SOCKS5 handshake context */
struct socks5_handshake_ctx_s {
    turbo_proxy_connect_ctx_t* proxy_ctx;
    socks5_handshake_state_t state;
    uint8_t send_buf[512];
    size_t send_len;
    uint8_t recv_buf[512];
    size_t recv_len;
    size_t recv_expected;
    int selected_method;
    uv_write_t write_req;
    uv_buf_t write_buf;
};

/* Timeout callback */
static void on_proxy_timeout(uv_timer_t* timer) {
    turbo_proxy_connect_ctx_t* ctx = (turbo_proxy_connect_ctx_t*)timer->data;

    if (!ctx) return;

    TLOG_ERROR("SOCKS5 proxy connection timeout");
    ctx->timed_out = 1;

    /* Notify connection failed */
    if (ctx->on_connect) {
        ctx->on_connect(ctx->client, UV_ETIMEDOUT, NULL);
    }

    /* Close the connection */
    turbo_tcp_client_close(ctx->client);

    /* Stop timer */
    uv_timer_stop(&ctx->timeout_timer);
    uv_close((uv_handle_t*)&ctx->timeout_timer, NULL);
}

/* Stop timeout timer */
static void stop_proxy_timeout(turbo_proxy_connect_ctx_t* ctx) {
    if (!ctx) return;

    if (uv_is_active((uv_handle_t*)&ctx->timeout_timer)) {
        uv_timer_stop(&ctx->timeout_timer);
        uv_close((uv_handle_t*)&ctx->timeout_timer, NULL);
    }
}

/* ── Async SOCKS5 Handshake ──────────────────────────────────── */

/* Forward declarations */
static void socks5_handshake_step(socks5_handshake_ctx_t* hs_ctx);
static void on_socks5_write(uv_write_t* req, int status);
static void on_socks5_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

/* Build method negotiation request */
static void build_method_request(socks5_handshake_ctx_t* hs_ctx) {
    turbo_socks5_config_t* config = &hs_ctx->proxy_ctx->proxy_config;
    uint8_t* buf = hs_ctx->send_buf;
    size_t pos = 0;

    buf[pos++] = SOCKS5_VERSION;
    if (config->auth_required) {
        buf[pos++] = 2;  /* 2 methods */
        buf[pos++] = SOCKS5_AUTH_NONE;
        buf[pos++] = SOCKS5_AUTH_USERPASS;
    } else {
        buf[pos++] = 1;  /* 1 method */
        buf[pos++] = SOCKS5_AUTH_NONE;
    }

    hs_ctx->send_len = pos;
    hs_ctx->recv_expected = 2;  /* VER + METHOD */
}

/* Build authentication request */
static void build_auth_request(socks5_handshake_ctx_t* hs_ctx) {
    turbo_socks5_config_t* config = &hs_ctx->proxy_ctx->proxy_config;
    uint8_t* buf = hs_ctx->send_buf;
    size_t pos = 0;

    size_t ulen = strlen(config->username);
    size_t plen = strlen(config->password);

    buf[pos++] = 0x01;  /* Auth version */
    buf[pos++] = (uint8_t)ulen;
    memcpy(buf + pos, config->username, ulen);
    pos += ulen;
    buf[pos++] = (uint8_t)plen;
    memcpy(buf + pos, config->password, plen);
    pos += plen;

    hs_ctx->send_len = pos;
    hs_ctx->recv_expected = 2;  /* VER + STATUS */
}

/* Build connect request */
static void build_connect_request(socks5_handshake_ctx_t* hs_ctx) {
    turbo_proxy_connect_ctx_t* ctx = hs_ctx->proxy_ctx;
    uint8_t* buf = hs_ctx->send_buf;
    size_t pos = 0;

    buf[pos++] = SOCKS5_VERSION;
    buf[pos++] = SOCKS5_CMD_CONNECT;
    buf[pos++] = 0x00;  /* Reserved */

    /* Check if target is IPv4 */
    struct in_addr addr;
    if (inet_pton(AF_INET, ctx->target_host, &addr) == 1) {
        buf[pos++] = SOCKS5_ATYP_IPV4;
        memcpy(buf + pos, &addr, 4);
        pos += 4;
    } else {
        /* Domain name */
        size_t len = strlen(ctx->target_host);
        buf[pos++] = SOCKS5_ATYP_DOMAIN;
        buf[pos++] = (uint8_t)len;
        memcpy(buf + pos, ctx->target_host, len);
        pos += len;
    }

    /* Port */
    uint16_t nport = htons(ctx->target_port);
    memcpy(buf + pos, &nport, 2);
    pos += 2;

    hs_ctx->send_len = pos;
    hs_ctx->recv_expected = 4;  /* VER + REP + RSV + ATYP (minimum) */
}

/* Allocation callback for uv_read_start */
static void on_socks5_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    socks5_handshake_ctx_t* hs_ctx = (socks5_handshake_ctx_t*)handle->data;
    buf->base = (char*)(hs_ctx->recv_buf + hs_ctx->recv_len);
    buf->len = sizeof(hs_ctx->recv_buf) - hs_ctx->recv_len;
}

/* Read callback */
static void on_socks5_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    (void)buf;
    socks5_handshake_ctx_t* hs_ctx = (socks5_handshake_ctx_t*)stream->data;
    turbo_proxy_connect_ctx_t* ctx = hs_ctx->proxy_ctx;

    if (nread < 0) {
        TLOG_ERROR("SOCKS5 read error: {}", uv_strerror((int)nread));
        uv_read_stop(stream);
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(ctx->client, (int)nread, NULL);
        }
        free(hs_ctx);
        free(ctx);
        return;
    }

    if (nread == 0) return;

    hs_ctx->recv_len += nread;

    /* Check if we have enough data */
    if (hs_ctx->recv_len >= hs_ctx->recv_expected) {
        uv_read_stop(stream);
        socks5_handshake_step(hs_ctx);
    }
}

/* Write callback */
static void on_socks5_write(uv_write_t* req, int status) {
    socks5_handshake_ctx_t* hs_ctx = (socks5_handshake_ctx_t*)req->data;
    turbo_proxy_connect_ctx_t* ctx = hs_ctx->proxy_ctx;

    if (status < 0) {
        TLOG_ERROR("SOCKS5 write error: {}", uv_strerror(status));
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(ctx->client, status, NULL);
        }
        free(hs_ctx);
        free(ctx);
        return;
    }

    /* Start reading response */
    hs_ctx->recv_len = 0;
    ctx->client->handle.data = hs_ctx;
    uv_read_start((uv_stream_t*)&ctx->client->handle, on_socks5_alloc, on_socks5_read);
}

/* Main handshake state machine */
static void socks5_handshake_step(socks5_handshake_ctx_t* hs_ctx) {
    turbo_proxy_connect_ctx_t* ctx = hs_ctx->proxy_ctx;
    int rc;

    switch (hs_ctx->state) {
        case SOCKS5_STATE_METHOD_SEND:
            build_method_request(hs_ctx);
            hs_ctx->state = SOCKS5_STATE_METHOD_RECV;
            hs_ctx->write_buf = uv_buf_init((char*)hs_ctx->send_buf, hs_ctx->send_len);
            hs_ctx->write_req.data = hs_ctx;
            rc = uv_write(&hs_ctx->write_req, (uv_stream_t*)&ctx->client->handle, &hs_ctx->write_buf, 1, on_socks5_write);
            if (rc != 0) {
                TLOG_ERROR("Failed to send method request: {}", uv_strerror(rc));
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, rc, NULL);
                free(hs_ctx);
                free(ctx);
            }
            break;

        case SOCKS5_STATE_METHOD_RECV:
            /* Parse method response */
            if (hs_ctx->recv_buf[0] != SOCKS5_VERSION) {
                TLOG_ERROR("Invalid SOCKS5 version: {}", hs_ctx->recv_buf[0]);
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, UV_EPROTO, NULL);
                free(hs_ctx);
                free(ctx);
                return;
            }

            hs_ctx->selected_method = hs_ctx->recv_buf[1];
            if (hs_ctx->selected_method == SOCKS5_AUTH_FAILED) {
                TLOG_ERROR("No acceptable auth methods");
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, UV_EACCES, NULL);
                free(hs_ctx);
                free(ctx);
                return;
            }

            /* Next state */
            if (hs_ctx->selected_method == SOCKS5_AUTH_USERPASS) {
                hs_ctx->state = SOCKS5_STATE_AUTH_SEND;
                socks5_handshake_step(hs_ctx);
            } else {
                hs_ctx->state = SOCKS5_STATE_CONNECT_SEND;
                socks5_handshake_step(hs_ctx);
            }
            break;

        case SOCKS5_STATE_AUTH_SEND:
            build_auth_request(hs_ctx);
            hs_ctx->state = SOCKS5_STATE_AUTH_RECV;
            hs_ctx->write_buf = uv_buf_init((char*)hs_ctx->send_buf, hs_ctx->send_len);
            hs_ctx->write_req.data = hs_ctx;
            rc = uv_write(&hs_ctx->write_req, (uv_stream_t*)&ctx->client->handle, &hs_ctx->write_buf, 1, on_socks5_write);
            if (rc != 0) {
                TLOG_ERROR("Failed to send auth request: {}", uv_strerror(rc));
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, rc, NULL);
                free(hs_ctx);
                free(ctx);
            }
            break;

        case SOCKS5_STATE_AUTH_RECV:
            /* Parse auth response */
            if (hs_ctx->recv_buf[1] != 0) {
                TLOG_ERROR("Authentication failed");
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, UV_EACCES, NULL);
                free(hs_ctx);
                free(ctx);
                return;
            }

            hs_ctx->state = SOCKS5_STATE_CONNECT_SEND;
            socks5_handshake_step(hs_ctx);
            break;

        case SOCKS5_STATE_CONNECT_SEND:
            build_connect_request(hs_ctx);
            hs_ctx->state = SOCKS5_STATE_CONNECT_RECV;
            hs_ctx->write_buf = uv_buf_init((char*)hs_ctx->send_buf, hs_ctx->send_len);
            hs_ctx->write_req.data = hs_ctx;
            rc = uv_write(&hs_ctx->write_req, (uv_stream_t*)&ctx->client->handle, &hs_ctx->write_buf, 1, on_socks5_write);
            if (rc != 0) {
                TLOG_ERROR("Failed to send connect request: {}", uv_strerror(rc));
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, rc, NULL);
                free(hs_ctx);
                free(ctx);
            }
            break;

        case SOCKS5_STATE_CONNECT_RECV: {
            /* Parse connect response */
            if (hs_ctx->recv_buf[0] != SOCKS5_VERSION) {
                TLOG_ERROR("Invalid SOCKS5 version in connect reply");
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, UV_EPROTO, NULL);
                free(hs_ctx);
                free(ctx);
                return;
            }

            if (hs_ctx->recv_buf[1] != SOCKS5_REP_SUCCESS) {
                TLOG_ERROR("SOCKS5 connect failed with code: {}", hs_ctx->recv_buf[1]);
                stop_proxy_timeout(ctx);
                if (ctx->on_connect) ctx->on_connect(ctx->client, UV_ECONNREFUSED, NULL);
                free(hs_ctx);
                free(ctx);
                return;
            }

            /* Need to read full address */
            uint8_t atyp = hs_ctx->recv_buf[3];
            size_t addr_len;
            switch (atyp) {
                case SOCKS5_ATYP_IPV4: addr_len = 4; break;
                case SOCKS5_ATYP_IPV6: addr_len = 16; break;
                case SOCKS5_ATYP_DOMAIN:
                    if (hs_ctx->recv_len < 5) {
                        /* Need more data */
                        hs_ctx->recv_expected = 5;
                        ctx->client->handle.data = hs_ctx;
                        uv_read_start((uv_stream_t*)&ctx->client->handle, on_socks5_alloc, on_socks5_read);
                        return;
                    }
                    addr_len = hs_ctx->recv_buf[4];
                    break;
                default:
                    TLOG_ERROR("Unsupported address type: {}", atyp);
                    stop_proxy_timeout(ctx);
                    if (ctx->on_connect) ctx->on_connect(ctx->client, UV_EPROTO, NULL);
                    free(hs_ctx);
                    free(ctx);
                    return;
            }

            size_t total_len = 4 + (atyp == SOCKS5_ATYP_DOMAIN ? 1 : 0) + addr_len + 2;
            if (hs_ctx->recv_len < total_len) {
                /* Need more data */
                hs_ctx->recv_expected = total_len;
                ctx->client->handle.data = hs_ctx;
                uv_read_start((uv_stream_t*)&ctx->client->handle, on_socks5_alloc, on_socks5_read);
                return;
            }

            /* Handshake complete! */
            TLOG_INFO("SOCKS5 handshake complete (async), connected to {}:{}", ctx->target_host, ctx->target_port);

            stop_proxy_timeout(ctx);

            /* Set callbacks */
            ctx->client->on_recv = ctx->on_recv;
            ctx->client->on_connect = ctx->on_connect;
            ctx->client->on_close = ctx->on_close;
            ctx->client->conn_state = 3;

            /* Start normal reading */
            rc = turbo_tcp_read_start(ctx->client);
            if (rc != 0) {
                TLOG_ERROR("Failed to start reading: {}", uv_strerror(rc));
            }

            /* Notify success */
            if (ctx->on_connect) {
                ctx->on_connect(ctx->client, 0, NULL);
            }

            free(hs_ctx);
            free(ctx);
            break;
        }

        default:
            TLOG_ERROR("Invalid SOCKS5 handshake state: {}", ENUM_NAME(hs_ctx->state));
            stop_proxy_timeout(ctx);
            if (ctx->on_connect) ctx->on_connect(ctx->client, UV_EPROTO, NULL);
            free(hs_ctx);
            free(ctx);
            break;
    }
}

/* Callback after SOCKS5 handshake completes */
static void on_proxy_handshake_complete(uv_write_t* req, int status) {
    turbo_proxy_connect_ctx_t* ctx = (turbo_proxy_connect_ctx_t*)req->data;

    if (status < 0) {
        TLOG_ERROR("SOCKS5 handshake write failed: {}", uv_strerror(status));
        if (ctx->on_connect) {
            ctx->on_connect(ctx->client, status, NULL);
        }
        free(ctx);
        free(req);
        return;
    }

    /* Handshake sent, now we need to read response */
    /* For simplicity, we'll do blocking handshake in connect callback */
    free(ctx);
    free(req);
}

/* Callback when connected to proxy server */
static void on_proxy_connected(uv_connect_t* req, int status) {
    turbo_proxy_connect_ctx_t* ctx = (turbo_proxy_connect_ctx_t*)req->data;
    turbo_tcp_client_t* client = ctx->client;

    free(req);

    /* Check if already timed out */
    if (ctx->timed_out) {
        free(ctx);
        return;
    }

    if (status < 0) {
        TLOG_ERROR("Failed to connect to SOCKS5 proxy: {}", uv_strerror(status));
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(client, status, NULL);
        }
        free(ctx);
        return;
    }

    TLOG_INFO("Connected to SOCKS5 proxy {}:{}", ctx->proxy_config.host, ctx->proxy_config.port);

    /* Allocate handshake context */
    socks5_handshake_ctx_t* hs_ctx = (socks5_handshake_ctx_t*)calloc(1, sizeof(socks5_handshake_ctx_t));
    if (!hs_ctx) {
        TLOG_ERROR("Failed to allocate handshake context");
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(client, UV_ENOMEM, NULL);
        }
        free(ctx);
        return;
    }

    hs_ctx->proxy_ctx = ctx;
    hs_ctx->state = SOCKS5_STATE_METHOD_SEND;
    ctx->handshake_ctx = hs_ctx;

    /* Start async handshake */
    socks5_handshake_step(hs_ctx);
}

/* Callback after DNS resolution for proxy */
static void on_proxy_dns_resolved(const char* hostname, const char* ip, void* user_data) {
    turbo_proxy_connect_ctx_t* ctx = (turbo_proxy_connect_ctx_t*)user_data;

    if (!ctx) {
        TLOG_ERROR("NULL context in on_proxy_dns_resolved");
        return;
    }

    /* Check if already timed out */
    if (ctx->timed_out) {
        free(ctx);
        return;
    }

    turbo_tcp_client_t* client = ctx->client;

    if (!ip) {
        TLOG_ERROR("Failed to resolve proxy hostname: {}", hostname);
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(client, UV_EAI_NONAME, NULL);
        }
        free(ctx);
        return;
    }

    TLOG_INFO("Resolved proxy {} to {}", hostname, ip);

    /* Connect to proxy server */
    struct sockaddr_in addr;
    int rc = uv_ip4_addr(ip, ctx->proxy_config.port, &addr);
    if (rc != 0) {
        TLOG_ERROR("Invalid proxy IP address: {}", ip);
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(client, rc, NULL);
        }
        free(ctx);
        return;
    }

    uv_connect_t* connect_req = (uv_connect_t*)malloc(sizeof(uv_connect_t));
    if (!connect_req) {
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(client, UV_ENOMEM, NULL);
        }
        free(ctx);
        return;
    }

    connect_req->data = ctx;
    client->conn_state = 2; /* Connecting */

    rc = uv_tcp_connect(connect_req, &client->handle, (const struct sockaddr*)&addr, on_proxy_connected);
    if (rc != 0) {
        TLOG_ERROR("Failed to initiate proxy connection: {}", uv_strerror(rc));
        stop_proxy_timeout(ctx);
        if (ctx->on_connect) {
            ctx->on_connect(client, rc, NULL);
        }
        free(connect_req);
        free(ctx);
    }
}

/**
 * @brief Connect via SOCKS5 proxy
 */
int turbo_tcp_client_connect_via_proxy(turbo_tcp_client_t* client,
                                       const char* host,
                                       unsigned short port,
                                       const turbo_socks5_config_t* proxy,
                                       turbo_recv_cb on_recv,
                                       turbo_connect_cb on_connect,
                                       turbo_close_cb on_close) {
    if (!client || !host || !proxy) return UV_EINVAL;

    if (client->conn_state != 0) {
        return UV_EALREADY;
    }

    /* Allocate context */
    turbo_proxy_connect_ctx_t* ctx = (turbo_proxy_connect_ctx_t*)calloc(1, sizeof(turbo_proxy_connect_ctx_t));
    if (!ctx) return UV_ENOMEM;

    ctx->client = client;
    strncpy(ctx->target_host, host, sizeof(ctx->target_host) - 1);
    ctx->target_host[sizeof(ctx->target_host) - 1] = '\0';
    ctx->target_port = port;
    ctx->proxy_config = *proxy;
    ctx->on_recv = on_recv;
    ctx->on_connect = on_connect;
    ctx->on_close = on_close;
    ctx->timed_out = 0;

    /* Initialize timeout timer if specified */
    if (proxy->timeout_ms > 0) {
        uv_timer_init(client->handle.loop, &ctx->timeout_timer);
        ctx->timeout_timer.data = ctx;
        uv_timer_start(&ctx->timeout_timer, on_proxy_timeout, proxy->timeout_ms, 0);
        TLOG_INFO("SOCKS5 proxy timeout set to {} ms", proxy->timeout_ms);
    }

    /* Initialize DNS subsystem */
    int rc = turbo_dns_init();
    if (rc != 0) {
        if (proxy->timeout_ms > 0) {
            uv_timer_stop(&ctx->timeout_timer);
            uv_close((uv_handle_t*)&ctx->timeout_timer, NULL);
        }
        free(ctx);
        return rc;
    }
    client->dns_initialized = 1;

    /* Resolve proxy hostname */
    client->conn_state = 1; /* Resolving */
    rc = turbo_dns_resolve_async(client->handle.loop, proxy->host, TURBO_DNS_ANY, on_proxy_dns_resolved, ctx);
    if (rc != 0) {
        client->conn_state = 0;
        if (proxy->timeout_ms > 0) {
            uv_timer_stop(&ctx->timeout_timer);
            uv_close((uv_handle_t*)&ctx->timeout_timer, NULL);
        }
        free(ctx);
        return rc;
    }

    return 0;
}
