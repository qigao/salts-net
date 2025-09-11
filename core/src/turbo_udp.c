/**
 * UDP Transport Implementation  
 * "UDP is simple. That's its beauty and its curse." - Linus
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "platform.h"
#include "log.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

// UDP transport data
typedef struct turbo_udp_data_s {
    uv_udp_t uv_handle;
    
    // UDP is connectionless, but we simulate connection for API consistency
    struct sockaddr_storage remote_addr;
    bool has_remote_addr;
    
    // For "connect" simulation
    bool connected;
} turbo_udp_data_t;

// Forward declarations
static int udp_init(turbo_handle_t* handle);
static int udp_connect(turbo_handle_t* handle, const char* address, int port);
static int udp_bind(turbo_handle_t* handle, const char* address, int port);
static int udp_read_start(turbo_handle_t* handle);
static int udp_read_stop(turbo_handle_t* handle);
static int udp_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
static int udp_close(turbo_handle_t* handle);
static int udp_cleanup(turbo_handle_t* handle);

// libuv callbacks
static void udp_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                       const struct sockaddr* addr, unsigned flags);
static void udp_send_cb(uv_udp_send_t* req, int status);
static void udp_close_cb(uv_handle_t* handle);

// Transport vtable
static const turbo_transport_vtable_t udp_vtable = {
    .init = udp_init,
    .connect = udp_connect,
    .bind = udp_bind,
    .listen = NULL,  // UDP doesn't listen in traditional sense
    .accept = NULL,  // UDP doesn't accept
    .read_start = udp_read_start,
    .read_stop = udp_read_stop,
    .write = udp_write,
    .close = udp_close,
    .cleanup = udp_cleanup,
    .set_option = NULL,
    .get_option = NULL
};

// =============================================================================
// Transport registration
// =============================================================================

void turbo_register_udp(void) {
    turbo_register_transport(TURBO_UDP, &udp_vtable);
}

// =============================================================================
// Transport implementation
// =============================================================================

static int udp_init(turbo_handle_t* handle) {
    turbo_udp_data_t* udp_data = malloc(sizeof(turbo_udp_data_t));
    if (udp_data == NULL) {
        return UV_ENOMEM;
    }
    
    memset(udp_data, 0, sizeof(*udp_data));
    
    int err = uv_udp_init(handle->loop, &udp_data->uv_handle);
    if (err != 0) {
        log_error("uv_udp_init failed: %s", uv_strerror(err));
        free(udp_data);
        return err;
    }
    
    udp_data->uv_handle.data = handle;
    
    turbo_internal_t* internal = turbo_get_internal(handle);
    internal->transport_data = udp_data;
    
    log_debug("UDP transport initialized");
    return 0;
}

static int udp_connect(turbo_handle_t* handle, const char* address, int port) {
    turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Parse and store remote address for later use - unified parsing
    struct sockaddr_storage addr;
    int err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    memcpy(&udp_data->remote_addr, &addr, sizeof(addr));
    udp_data->has_remote_addr = true;
    udp_data->connected = true;
    
    // Copy address info
    strncpy(handle->remote_ip, address, sizeof(handle->remote_ip) - 1);
    handle->remote_port = port;
    
    // UDP "connection" is immediate
    handle->state = TURBO_CONNECTED;
    handle->connect_time = turbo_hrtime();
    
    log_info("UDP connected to %s:%d", address, port);
    
    // Call callback immediately
    if (handle->connect_cb) {
        handle->connect_cb(handle, 0);
    }
    
    return 0;
}

static int udp_bind(turbo_handle_t* handle, const char* address, int port) {
    turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Bind using unified address parsing - supports IPv4, IPv6
    struct sockaddr_storage addr;
    int err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    err = uv_udp_bind(&udp_data->uv_handle, (const struct sockaddr*)&addr, 0);
    if (err != 0) {
        log_error("uv_udp_bind failed: %s", uv_strerror(err));
        return err;
    }
    
    // Copy address info
    strncpy(handle->local_ip, address, sizeof(handle->local_ip) - 1);
    handle->local_port = port;
    
    handle->state = TURBO_CONNECTED; // UDP is "connected" when bound
    
    log_debug("UDP bound to %s:%d", address, port);
    return 0;
}

static int udp_read_start(turbo_handle_t* handle) {
    turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_udp_recv_start(&udp_data->uv_handle, udp_alloc_cb, udp_recv_cb);
    if (err != 0) {
        log_error("uv_udp_recv_start failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("UDP read started");
    return 0;
}

static int udp_read_stop(turbo_handle_t* handle) {
    turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_udp_recv_stop(&udp_data->uv_handle);
    if (err != 0) {
        log_error("uv_udp_recv_stop failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("UDP read stopped");
    return 0;
}

static int udp_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs) {
    turbo_handle_t* handle = req->handle;
    turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (!udp_data->has_remote_addr) {
        log_error("UDP write without remote address");
        return UV_ENOTCONN;
    }
    
    // Allocate libuv send request
    uv_udp_send_t* uv_req = malloc(sizeof(uv_udp_send_t));
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
    
    req->internal = uv_req;
    
    int err = uv_udp_send(uv_req, &udp_data->uv_handle, uv_bufs, nbufs,
                         (const struct sockaddr*)&udp_data->remote_addr, udp_send_cb);
    if (err != 0) {
        log_error("uv_udp_send failed: %s", uv_strerror(err));
        free(uv_bufs);
        free(uv_req);
        return err;
    }
    
    free(uv_bufs);
    log_debug("UDP send initiated for %d buffers", nbufs);
    return 0;
}

static int udp_close(turbo_handle_t* handle) {
    turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
    
    uv_close((uv_handle_t*)&udp_data->uv_handle, udp_close_cb);
    
    log_debug("UDP close initiated");
    return 0;
}

static int udp_cleanup(turbo_handle_t* handle) {
    turbo_internal_t* internal = turbo_get_internal(handle);
    if (internal->transport_data) {
        free(internal->transport_data);
        internal->transport_data = NULL;
    }
    
    log_debug("UDP cleanup completed");
    return 0;
}

// =============================================================================
// libuv callbacks
// =============================================================================

static void udp_alloc_cb(uv_handle_t* uv_handle, size_t suggested_size, uv_buf_t* buf) {
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

static void udp_recv_cb(uv_udp_t* uv_handle, ssize_t nread, const uv_buf_t* buf,
                       const struct sockaddr* addr, unsigned flags) {
    turbo_handle_t* handle = (turbo_handle_t*)uv_handle->data;
    
    if (nread > 0) {
        handle->bytes_read += nread;
        
        turbo_udp_data_t* udp_data = (turbo_udp_data_t*)turbo_get_internal(handle)->transport_data;
        
        // For server mode, store sender address for echo back
        if (!udp_data->connected && addr) {
            memcpy(&udp_data->remote_addr, addr, sizeof(struct sockaddr_storage));
            udp_data->has_remote_addr = true;
            log_debug("UDP server stored sender address for echo");
        }
        
        // For connected UDP, only accept data from the connected peer
        if (udp_data->connected) {
            // Simple address comparison (this could be more robust)
            if (addr && memcmp(addr, &udp_data->remote_addr, sizeof(struct sockaddr)) != 0) {
                log_debug("UDP received data from unexpected sender, ignoring");
                if (buf->base) free(buf->base);
                return;
            }
        }
        
        if (handle->read_cb) {
            turbo_buf_t turbo_buf;
            turbo_buf.base = buf->base;
            turbo_buf.len = buf->len;
            
            handle->read_cb(handle, nread, &turbo_buf);
        }
    }
    
    // Always free the buffer
    if (buf->base) {
        free(buf->base);
    }
}

static void udp_send_cb(uv_udp_send_t* req, int status) {
    turbo_req_t* turbo_req = (turbo_req_t*)req->data;
    
    if (turbo_req->write_cb) {
        turbo_req->write_cb(turbo_req, status);
    }
    
    // Cleanup
    free(req);
    turbo_req->internal = NULL;
}

static void udp_close_cb(uv_handle_t* handle) {
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
    log_debug("UDP handle closed and cleaned up");
}