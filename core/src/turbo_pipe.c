/**
 * PIPE Transport Implementation
 * "PIPE is just TCP without the network bullshit" - Linus
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "platform.h"
#include "log.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

// PIPE transport data - what goes in turbo_internal_t.transport_data
typedef struct turbo_pipe_data_s {
    uv_pipe_t uv_handle;
    uv_connect_t connect_req;
    
    // Server mode
    bool is_server;
    
    // Write requests tracking
    uv_write_t* write_reqs;
    int write_req_count;
} turbo_pipe_data_t;

// Forward declarations
static int pipe_init(turbo_handle_t* handle);
static int pipe_connect(turbo_handle_t* handle, const char* address, int port);
static int pipe_bind(turbo_handle_t* handle, const char* address, int port);
static int pipe_listen(turbo_handle_t* handle, int backlog);
static int pipe_accept(turbo_handle_t* server, turbo_handle_t* client);
static int pipe_read_start(turbo_handle_t* handle);
static int pipe_read_stop(turbo_handle_t* handle);
static int pipe_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
static int pipe_close(turbo_handle_t* handle);
static int pipe_cleanup(turbo_handle_t* handle);

// libuv callbacks
static void pipe_connect_cb(uv_connect_t* req, int status);
static void pipe_connection_cb(uv_stream_t* server, int status);
static void pipe_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void pipe_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
static void pipe_write_cb(uv_write_t* req, int status);
static void pipe_close_cb(uv_handle_t* handle);

// Transport vtable
static const turbo_transport_vtable_t pipe_vtable = {
    .init = pipe_init,
    .connect = pipe_connect,
    .bind = pipe_bind,
    .listen = pipe_listen,
    .accept = pipe_accept,
    .read_start = pipe_read_start,
    .read_stop = pipe_read_stop,
    .write = pipe_write,
    .close = pipe_close,
    .cleanup = pipe_cleanup,
    .set_option = NULL,  // TODO: implement PIPE-specific options
    .get_option = NULL
};

// =============================================================================
// Transport registration  
// =============================================================================

void turbo_register_pipe(void) {
    turbo_register_transport(TURBO_PIPE, &pipe_vtable);
}

// =============================================================================
// Transport implementation - the meat and potatoes
// =============================================================================

static int pipe_init(turbo_handle_t* handle) {
    turbo_pipe_data_t* pipe_data = malloc(sizeof(turbo_pipe_data_t));
    if (pipe_data == NULL) {
        return UV_ENOMEM;
    }
    
    memset(pipe_data, 0, sizeof(*pipe_data));
    
    // ipc=0 means regular named pipe, not IPC channel
    int err = uv_pipe_init(handle->loop, &pipe_data->uv_handle, 0);
    if (err != 0) {
        log_error("uv_pipe_init failed: %s", uv_strerror(err));
        free(pipe_data);
        return err;
    }
    
    // Link back to our handle
    pipe_data->uv_handle.data = handle;
    
    turbo_internal_t* internal = turbo_get_internal(handle);
    internal->transport_data = pipe_data;
    
    log_debug("PIPE transport initialized");
    return 0;
}

static int pipe_connect(turbo_handle_t* handle, const char* address, int port) {
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
    // For PIPE, port parameter is ignored - address is the pipe path
    UNUSED(port);
    
    pipe_data->connect_req.data = handle;
    uv_pipe_connect(&pipe_data->connect_req, &pipe_data->uv_handle,
                    address, pipe_connect_cb);
    
    // Store the pipe path in remote_ip for consistency
    strncpy(handle->remote_ip, address, sizeof(handle->remote_ip) - 1);
    handle->remote_ip[sizeof(handle->remote_ip) - 1] = '\0';
    handle->remote_port = 0;  // No port for pipes
    
    log_debug("PIPE connect initiated to %s", address);
    return 0;
}

static int pipe_bind(turbo_handle_t* handle, const char* address, int port) {
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
    // For PIPE, port parameter is ignored - address is the pipe path
    UNUSED(port);
    
    int err = uv_pipe_bind(&pipe_data->uv_handle, address);
    if (err != 0) {
        log_error("uv_pipe_bind failed: %s", uv_strerror(err));
        return err;
    }
    
    pipe_data->is_server = true;
    
    // Store the pipe path in local_ip for consistency
    strncpy(handle->local_ip, address, sizeof(handle->local_ip) - 1);
    handle->local_ip[sizeof(handle->local_ip) - 1] = '\0';
    handle->local_port = 0;  // No port for pipes
    
    log_debug("PIPE bound to %s", address);
    return 0;
}

static int pipe_listen(turbo_handle_t* handle, int backlog) {
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_listen((uv_stream_t*)&pipe_data->uv_handle, backlog, pipe_connection_cb);
    if (err != 0) {
        log_error("uv_listen failed: %s", uv_strerror(err));
        return err;
    }
    
    handle->state = TURBO_CONNECTED; // Server is "connected" when listening
    log_debug("PIPE listening with backlog %d", backlog);
    return 0;
}

static int pipe_accept(turbo_handle_t* server, turbo_handle_t* client) {
    turbo_pipe_data_t* server_data = (turbo_pipe_data_t*)turbo_get_internal(server)->transport_data;
    turbo_pipe_data_t* client_data = (turbo_pipe_data_t*)turbo_get_internal(client)->transport_data;
    
    int err = uv_accept((uv_stream_t*)&server_data->uv_handle, 
                       (uv_stream_t*)&client_data->uv_handle);
    if (err != 0) {
        log_error("uv_accept failed: %s", uv_strerror(err));
        return err;
    }
    
    client->state = TURBO_CONNECTED;
    
    // For pipes, we can't get peer info like IP addresses
    // Just mark it as a pipe connection
    strcpy(client->remote_ip, "pipe-client");
    client->remote_port = 0;
    
    log_debug("PIPE client accepted");
    return 0;
}

static int pipe_read_start(turbo_handle_t* handle) {
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_read_start((uv_stream_t*)&pipe_data->uv_handle, pipe_alloc_cb, pipe_read_cb);
    if (err != 0) {
        log_error("uv_read_start failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("PIPE read started");
    return 0;
}

static int pipe_read_stop(turbo_handle_t* handle) {
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_read_stop((uv_stream_t*)&pipe_data->uv_handle);
    if (err != 0) {
        log_error("uv_read_stop failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("PIPE read stopped");
    return 0;
}

static int pipe_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs) {
    turbo_handle_t* handle = req->handle;
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
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
    
    int err = uv_write(uv_req, (uv_stream_t*)&pipe_data->uv_handle,
                      uv_bufs, nbufs, pipe_write_cb);
    if (err != 0) {
        log_error("uv_write failed: %s", uv_strerror(err));
        free(uv_bufs);
        free(uv_req);
        return err;
    }
    
    // uv_bufs will be freed in callback
    free(uv_bufs);
    log_debug("PIPE write initiated for %d buffers", nbufs);
    return 0;
}

static int pipe_close(turbo_handle_t* handle) {
    turbo_pipe_data_t* pipe_data = (turbo_pipe_data_t*)turbo_get_internal(handle)->transport_data;
    
    uv_close((uv_handle_t*)&pipe_data->uv_handle, pipe_close_cb);
    
    log_debug("PIPE close initiated");
    return 0;
}

static int pipe_cleanup(turbo_handle_t* handle) {
    turbo_internal_t* internal = turbo_get_internal(handle);
    if (internal->transport_data) {
        free(internal->transport_data);
        internal->transport_data = NULL;
    }
    
    log_debug("PIPE cleanup completed");
    return 0;
}

// =============================================================================
// libuv callbacks - bridge between libuv and TurboNet
// =============================================================================

static void pipe_connect_cb(uv_connect_t* req, int status) {
    turbo_handle_t* handle = (turbo_handle_t*)req->data;
    
    if (status == 0) {
        handle->state = TURBO_CONNECTED;
        handle->connect_time = turbo_hrtime();
        
        // For pipes, local info is just the pipe name
        strcpy(handle->local_ip, "pipe-local");
        handle->local_port = 0;
        
        log_info("PIPE connected successfully to %s", handle->remote_ip);
    } else {
        handle->state = TURBO_ERROR;
        log_error("PIPE connect failed: %s", uv_strerror(status));
    }
    
    if (handle->connect_cb) {
        handle->connect_cb(handle, status);
    }
}

static void pipe_connection_cb(uv_stream_t* server, int status) {
    turbo_handle_t* handle = (turbo_handle_t*)server->data;
    
    log_debug("PIPE connection callback: status=%d", status);
    
    if (handle->connect_cb) {
        handle->connect_cb(handle, status);
    }
}

static void pipe_alloc_cb(uv_handle_t* uv_handle, size_t suggested_size, uv_buf_t* buf) {
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

static void pipe_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
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

static void pipe_write_cb(uv_write_t* req, int status) {
    turbo_req_t* turbo_req = (turbo_req_t*)req->data;
    
    if (turbo_req->write_cb) {
        turbo_req->write_cb(turbo_req, status);
    }
    
    // Cleanup
    free(req);
    turbo_req->internal = NULL;
}

static void pipe_close_cb(uv_handle_t* handle) {
    turbo_handle_t* turbo_handle = (turbo_handle_t*)handle->data;
    turbo_internal_t* internal = turbo_get_internal(turbo_handle);

    if (turbo_handle->close_cb) {
        turbo_handle->close_cb(turbo_handle);
    }

    if (internal && internal->vtable && internal->vtable->cleanup) {
        internal->vtable->cleanup(turbo_handle);
    }
    if (internal) {
        free(internal);
        turbo_handle->internal = NULL;
    }

    turbo_handle->state = TURBO_CLOSED;
    log_debug("PIPE handle closed and cleaned up");
}