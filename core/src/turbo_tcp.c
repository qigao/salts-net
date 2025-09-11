/**
 * TCP Transport Implementation
 * "TCP is boring and that's why it works" - Linus
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "platform.h"
#include "log.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

// TCP transport data - what goes in turbo_internal_t.transport_data
typedef struct turbo_tcp_data_s {
    uv_tcp_t uv_handle;
    uv_connect_t connect_req;
    
    // Server mode
    bool is_server;
    
    // Write requests tracking
    uv_write_t* write_reqs;
    int write_req_count;
} turbo_tcp_data_t;

// Forward declarations
static int tcp_init(turbo_handle_t* handle);
static int tcp_connect(turbo_handle_t* handle, const char* address, int port);
static int tcp_bind(turbo_handle_t* handle, const char* address, int port);
static int tcp_listen(turbo_handle_t* handle, int backlog);
static int tcp_accept(turbo_handle_t* server, turbo_handle_t* client);
static int tcp_read_start(turbo_handle_t* handle);
static int tcp_read_stop(turbo_handle_t* handle);
static int tcp_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
static int tcp_close(turbo_handle_t* handle);
static int tcp_cleanup(turbo_handle_t* handle);

// libuv callbacks
static void tcp_connect_cb(uv_connect_t* req, int status);
static void tcp_connection_cb(uv_stream_t* server, int status);
static void tcp_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void tcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
static void tcp_write_cb(uv_write_t* req, int status);
static void tcp_close_cb(uv_handle_t* handle);

// Transport vtable
static const turbo_transport_vtable_t tcp_vtable = {
    .init = tcp_init,
    .connect = tcp_connect,
    .bind = tcp_bind,
    .listen = tcp_listen,
    .accept = tcp_accept,
    .read_start = tcp_read_start,
    .read_stop = tcp_read_stop,
    .write = tcp_write,
    .close = tcp_close,
    .cleanup = tcp_cleanup,
    .set_option = NULL,  // TODO: implement TCP-specific options
    .get_option = NULL
};

// =============================================================================
// Transport registration 
// =============================================================================

void turbo_register_tcp(void) {
    turbo_register_transport(TURBO_TCP, &tcp_vtable);
}

// =============================================================================
// Transport implementation - the meat and potatoes
// =============================================================================

static int tcp_init(turbo_handle_t* handle) {
    turbo_tcp_data_t* tcp_data = malloc(sizeof(turbo_tcp_data_t));
    if (tcp_data == NULL) {
        return UV_ENOMEM;
    }
    
    memset(tcp_data, 0, sizeof(*tcp_data));
    
    int err = uv_tcp_init(handle->loop, &tcp_data->uv_handle);
    if (err != 0) {
        log_error("uv_tcp_init failed: %s", uv_strerror(err));
        free(tcp_data);
        return err;
    }
    
    // Link back to our handle
    tcp_data->uv_handle.data = handle;
    
    turbo_internal_t* internal = turbo_get_internal(handle);
    internal->transport_data = tcp_data;
    
    log_debug("TCP transport initialized");
    return 0;
}

static int tcp_connect(turbo_handle_t* handle, const char* address, int port) {
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Connect using unified address parsing - supports IPv4, IPv6
    struct sockaddr_storage addr;
    int err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    tcp_data->connect_req.data = handle;
    err = uv_tcp_connect(&tcp_data->connect_req, &tcp_data->uv_handle,
                        (const struct sockaddr*)&addr, tcp_connect_cb);
    
    if (err != 0) {
        log_error("uv_tcp_connect failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("TCP connect initiated to %s:%d", address, port);
    return 0;
}

static int tcp_bind(turbo_handle_t* handle, const char* address, int port) {
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Bind using unified address parsing - supports IPv4, IPv6
    struct sockaddr_storage addr;
    int err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    err = uv_tcp_bind(&tcp_data->uv_handle, (const struct sockaddr*)&addr, 0);
    if (err != 0) {
        log_error("uv_tcp_bind failed: %s", uv_strerror(err));
        return err;
    }
    
    tcp_data->is_server = true;
    log_debug("TCP bound to %s:%d", address, port);
    return 0;
}

static int tcp_listen(turbo_handle_t* handle, int backlog) {
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_listen((uv_stream_t*)&tcp_data->uv_handle, backlog, tcp_connection_cb);
    if (err != 0) {
        log_error("uv_listen failed: %s", uv_strerror(err));
        return err;
    }
    
    handle->state = TURBO_CONNECTED; // Server is "connected" when listening
    log_debug("TCP listening with backlog %d", backlog);
    return 0;
}

static int tcp_accept(turbo_handle_t* server, turbo_handle_t* client) {
    turbo_tcp_data_t* server_data = (turbo_tcp_data_t*)turbo_get_internal(server)->transport_data;
    turbo_tcp_data_t* client_data = (turbo_tcp_data_t*)turbo_get_internal(client)->transport_data;
    
    int err = uv_accept((uv_stream_t*)&server_data->uv_handle, 
                       (uv_stream_t*)&client_data->uv_handle);
    if (err != 0) {
        log_error("uv_accept failed: %s", uv_strerror(err));
        return err;
    }
    
    client->state = TURBO_CONNECTED;
    
    // Get peer address for statistics
    struct sockaddr_storage addr;
    int addrlen = sizeof(addr);
    err = uv_tcp_getpeername(&client_data->uv_handle, (struct sockaddr*)&addr, &addrlen);
    if (err == 0) {
        if (addr.ss_family == AF_INET) {
            struct sockaddr_in* addr4 = (struct sockaddr_in*)&addr;
            uv_ip4_name(addr4, client->remote_ip, sizeof(client->remote_ip));
            client->remote_port = ntohs(addr4->sin_port);
        }
    }
    
    log_debug("TCP client accepted from %s:%d", client->remote_ip, client->remote_port);
    return 0;
}

static int tcp_read_start(turbo_handle_t* handle) {
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_read_start((uv_stream_t*)&tcp_data->uv_handle, tcp_alloc_cb, tcp_read_cb);
    if (err != 0) {
        log_error("uv_read_start failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("TCP read started");
    return 0;
}

static int tcp_read_stop(turbo_handle_t* handle) {
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_read_stop((uv_stream_t*)&tcp_data->uv_handle);
    if (err != 0) {
        log_error("uv_read_stop failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("TCP read stopped");
    return 0;
}

static int tcp_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs) {
    turbo_handle_t* handle = req->handle;
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Allocate libuv write request
    uv_write_t* uv_req = malloc(sizeof(uv_write_t));
    if (uv_req == NULL) {
        return UV_ENOMEM;
    }
    
    uv_req->data = req;
    
    // Convert turbo_buf_t to uv_buf_t
    uv_buf_t* uv_bufs = malloc(sizeof(uv_buf_t) * nbufs);
    if (uv_bufs == NULL) {
        free(uv_req);
        return UV_ENOMEM;
    }
    
    for (unsigned int i = 0; i < nbufs; i++) {
        uv_bufs[i].base = bufs[i].base;
        uv_bufs[i].len = bufs[i].len;
    }
    
    req->internal = uv_req; // Store for cleanup
    
    int err = uv_write(uv_req, (uv_stream_t*)&tcp_data->uv_handle,
                      uv_bufs, nbufs, tcp_write_cb);
    if (err != 0) {
        log_error("uv_write failed: %s", uv_strerror(err));
        free(uv_bufs);
        free(uv_req);
        return err;
    }
    
    // uv_bufs will be freed in callback
    free(uv_bufs);
    log_debug("TCP write initiated for %d buffers", nbufs);
    return 0;
}

static int tcp_close(turbo_handle_t* handle) {
    turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    uv_close((uv_handle_t*)&tcp_data->uv_handle, tcp_close_cb);
    
    log_debug("TCP close initiated");
    return 0;
}

static int tcp_cleanup(turbo_handle_t* handle) {
    turbo_internal_t* internal = turbo_get_internal(handle);
    if (internal->transport_data) {
        free(internal->transport_data);
        internal->transport_data = NULL;
    }
    
    log_debug("TCP cleanup completed");
    return 0;
}

// =============================================================================
// libuv callbacks - bridge between libuv and TurboNet
// =============================================================================

static void tcp_connect_cb(uv_connect_t* req, int status) {
    turbo_handle_t* handle = (turbo_handle_t*)req->data;
    
    if (status == 0) {
        handle->state = TURBO_CONNECTED;
        handle->connect_time = turbo_hrtime();
        
        // Get local address for statistics
        turbo_tcp_data_t* tcp_data = (turbo_tcp_data_t*)turbo_get_internal(handle)->transport_data;
        struct sockaddr_storage addr;
        int addrlen = sizeof(addr);
        int err = uv_tcp_getsockname(&tcp_data->uv_handle, (struct sockaddr*)&addr, &addrlen);
        if (err == 0) {
            if (addr.ss_family == AF_INET) {
                struct sockaddr_in* addr4 = (struct sockaddr_in*)&addr;
                uv_ip4_name(addr4, handle->local_ip, sizeof(handle->local_ip));
                handle->local_port = ntohs(addr4->sin_port);
            }
        }
        
        log_info("TCP connected successfully to %s:%d", handle->remote_ip, handle->remote_port);
    } else {
        handle->state = TURBO_ERROR;
        log_error("TCP connect failed: %s", uv_strerror(status));
    }
    
    if (handle->connect_cb) {
        handle->connect_cb(handle, status);
    }
}

static void tcp_connection_cb(uv_stream_t* server, int status) {
    turbo_handle_t* handle = (turbo_handle_t*)server->data;
    
    log_debug("TCP connection callback: status=%d", status);
    
    if (handle->connect_cb) {
        handle->connect_cb(handle, status);
    }
}

static void tcp_alloc_cb(uv_handle_t* uv_handle, size_t suggested_size, uv_buf_t* buf) {
    turbo_handle_t* handle = (turbo_handle_t*)uv_handle->data;
    
    turbo_buf_t turbo_buf;
    if (handle->alloc_cb) {
        handle->alloc_cb(handle, suggested_size, &turbo_buf);
        buf->base = turbo_buf.base;
        buf->len = turbo_buf.len;
    } else {
        // Default allocation
        buf->base = malloc(suggested_size);
        buf->len = buf->base ? suggested_size : 0;
    }
}

static void tcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    turbo_handle_t* handle = (turbo_handle_t*)stream->data;
    
    if (nread > 0) {
        handle->bytes_read += nread;
    }
    
    if (handle->read_cb) {
        turbo_buf_t turbo_buf;
        turbo_buf.base = buf->base;
        turbo_buf.len = buf->len;
        
        handle->read_cb(handle, nread, &turbo_buf);
    }
    
    // Always free the buffer (user should copy data if needed)
    if (buf->base) {
        free(buf->base);
    }
}

static void tcp_write_cb(uv_write_t* req, int status) {
    turbo_req_t* turbo_req = (turbo_req_t*)req->data;
    
    if (turbo_req->write_cb) {
        turbo_req->write_cb(turbo_req, status);
    }
    
    // Cleanup
    free(req);
    turbo_req->internal = NULL;
}

static void tcp_close_cb(uv_handle_t* handle) {
    turbo_handle_t* turbo_handle = (turbo_handle_t*)handle->data;
    turbo_internal_t* internal = turbo_get_internal(turbo_handle);

    // Call the user's close callback first, if any
    if (turbo_handle->close_cb) {
        turbo_handle->close_cb(turbo_handle);
    }

    // Now, perform the actual cleanup of internal resources
    if (internal && internal->vtable && internal->vtable->cleanup) {
        internal->vtable->cleanup(turbo_handle); // Frees turbo_tcp_data_t
    }
    if (internal) {
        free(internal); // Frees turbo_internal_t
        turbo_handle->internal = NULL;
    }

    turbo_handle->state = TURBO_CLOSED;
    log_debug("TCP handle closed and cleaned up");
}