/**
 * KCP Transport Implementation  
 * "KCP is UDP with reliability - keep it simple" - Linus
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "platform.h"
#include "log.h"
#include <uv.h>
#include <ikcp.h>
#include <stdlib.h>
#include <string.h>

// KCP transport data - what goes in turbo_internal_t.transport_data
typedef struct turbo_kcp_data_s {
    uv_udp_t udp_handle;        // Underlying UDP socket
    uv_timer_t update_timer;    // KCP update timer
    ikcpcb* kcp;                // KCP control block
    
    // Connection info
    struct sockaddr_storage remote_addr;
    int remote_addrlen;
    
    // Server mode  
    bool is_server;
    bool connected;
    
    // Configuration
    IUINT32 conv;               // Conversation ID
    int update_interval;        // Timer interval (ms)
    
    // Buffers
    char recv_buffer[65536];    // UDP receive buffer
    char send_buffer[65536];    // KCP output buffer
} turbo_kcp_data_t;

// Forward declarations
static int kcp_init(turbo_handle_t* handle);
static int kcp_connect(turbo_handle_t* handle, const char* address, int port);
static int kcp_bind(turbo_handle_t* handle, const char* address, int port);
static int kcp_listen(turbo_handle_t* handle, int backlog);
static int kcp_accept(turbo_handle_t* server, turbo_handle_t* client);
static int kcp_read_start(turbo_handle_t* handle);
static int kcp_read_stop(turbo_handle_t* handle);
static int kcp_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
static int kcp_close(turbo_handle_t* handle);
static int kcp_cleanup(turbo_handle_t* handle);

// KCP callbacks
static int kcp_output_cb(const char *buf, int len, ikcpcb *kcp, void *user);
static void kcp_update_timer_cb(uv_timer_t* timer);

// libuv callbacks
static void kcp_udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                           const struct sockaddr* addr, unsigned flags);
static void kcp_udp_send_cb(uv_udp_send_t* req, int status);
static void kcp_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void kcp_close_cb(uv_handle_t* handle);

// Transport vtable
static const turbo_transport_vtable_t kcp_vtable = {
    .init = kcp_init,
    .connect = kcp_connect,
    .bind = kcp_bind,
    .listen = kcp_listen,
    .accept = kcp_accept,
    .read_start = kcp_read_start,
    .read_stop = kcp_read_stop,
    .write = kcp_write,
    .close = kcp_close,
    .cleanup = kcp_cleanup,
    .set_option = NULL,  // TODO: implement KCP-specific options
    .get_option = NULL
};

// =============================================================================
// Transport registration  
// =============================================================================

void turbo_register_kcp(void) {
    turbo_register_transport(TURBO_KCP, &kcp_vtable);
}

// =============================================================================
// KCP utility functions
// =============================================================================

static IUINT32 kcp_get_current_time(void) {
    return (IUINT32)turbo_ns_to_ms(turbo_hrtime()); // Use unified time functions
}

// Generate conversation ID from address:port for clients
static IUINT32 kcp_generate_conv(const char* address, int port) {
    IUINT32 hash = 0x12345678;
    
    // Simple hash of address and port
    while (*address) {
        hash = hash * 33 + *address++;
    }
    hash = hash * 33 + port;
    
    return hash & 0x7FFFFFFF; // Keep it positive
}

// =============================================================================
// Transport implementation - the meat and potatoes  
// =============================================================================

static int kcp_init(turbo_handle_t* handle) {
    turbo_kcp_data_t* kcp_data = malloc(sizeof(turbo_kcp_data_t));
    if (kcp_data == NULL) {
        return UV_ENOMEM;
    }
    
    memset(kcp_data, 0, sizeof(*kcp_data));
    
    // Initialize UDP handle
    int err = uv_udp_init(handle->loop, &kcp_data->udp_handle);
    if (err != 0) {
        log_error("uv_udp_init failed: %s", uv_strerror(err));
        free(kcp_data);
        return err;
    }
    
    // Initialize update timer
    err = uv_timer_init(handle->loop, &kcp_data->update_timer);
    if (err != 0) {
        log_error("uv_timer_init failed: %s", uv_strerror(err));
        free(kcp_data);
        return err;
    }
    
    // Link back to our handle
    kcp_data->udp_handle.data = handle;
    kcp_data->update_timer.data = handle;
    
    // Default configuration
    kcp_data->conv = 0;
    kcp_data->update_interval = 10; // 10ms default update interval
    
    turbo_internal_t* internal = turbo_get_internal(handle);
    internal->transport_data = kcp_data;
    
    log_debug("KCP transport initialized");
    return 0;
}

static int kcp_connect(turbo_handle_t* handle, const char* address, int port) {
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Generate conversation ID for client
    kcp_data->conv = kcp_generate_conv(address, port);
    
    // Store remote address - unified parsing
    struct sockaddr_storage addr;
    int err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    memcpy(&kcp_data->remote_addr, &addr, sizeof(addr));
    if (addr.ss_family == AF_INET) {
        kcp_data->remote_addrlen = sizeof(struct sockaddr_in);
    } else {
        kcp_data->remote_addrlen = sizeof(struct sockaddr_in6);
    }
    
    // Create KCP instance
    kcp_data->kcp = ikcp_create(kcp_data->conv, handle);
    if (kcp_data->kcp == NULL) {
        log_error("ikcp_create failed");
        return UV_ENOMEM;
    }
    
    // Set KCP output callback
    ikcp_setoutput(kcp_data->kcp, kcp_output_cb);
    
    // Configure KCP for optimal performance
    ikcp_nodelay(kcp_data->kcp, 1, 10, 2, 1); // Fast mode
    ikcp_wndsize(kcp_data->kcp, 128, 128);    // Window size
    
    // Bind UDP socket for sending - use any address (0.0.0.0 or ::)
    struct sockaddr_storage local_addr;
    if (addr.ss_family == AF_INET6) {
        turbo_parse_address("::", 0, &local_addr);
    } else {
        turbo_parse_address("0.0.0.0", 0, &local_addr);
    }
    err = uv_udp_bind(&kcp_data->udp_handle, (const struct sockaddr*)&local_addr, 0);
    if (err != 0) {
        log_error("uv_udp_bind failed: %s", uv_strerror(err));
        return err;
    }
    
    // Start update timer
    err = uv_timer_start(&kcp_data->update_timer, kcp_update_timer_cb,
                        kcp_data->update_interval, kcp_data->update_interval);
    if (err != 0) {
        log_error("uv_timer_start failed: %s", uv_strerror(err));
        return err;
    }
    
    kcp_data->connected = true;
    handle->state = TURBO_CONNECTED;
    
    // Store address info
    strncpy(handle->remote_ip, address, sizeof(handle->remote_ip) - 1);
    handle->remote_ip[sizeof(handle->remote_ip) - 1] = '\0';
    handle->remote_port = port;
    
    log_debug("KCP connect initiated to %s:%d (conv=%08x)", address, port, kcp_data->conv);
    
    // Trigger connect callback
    if (handle->connect_cb) {
        handle->connect_cb(handle, 0);
    }
    
    return 0;
}

static int kcp_bind(turbo_handle_t* handle, const char* address, int port) {
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Bind using unified address parsing - supports IPv4, IPv6  
    struct sockaddr_storage addr;
    int err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    err = uv_udp_bind(&kcp_data->udp_handle, (const struct sockaddr*)&addr, 0);
    if (err != 0) {
        log_error("uv_udp_bind failed: %s", uv_strerror(err));
        return err;
    }
    
    kcp_data->is_server = true;
    
    // Store address info
    strncpy(handle->local_ip, address, sizeof(handle->local_ip) - 1);
    handle->local_ip[sizeof(handle->local_ip) - 1] = '\0';
    handle->local_port = port;
    
    log_debug("KCP bound to %s:%d", address, port);
    return 0;
}

static int kcp_listen(turbo_handle_t* handle, int backlog) {
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    UNUSED(backlog); // KCP server doesn't use backlog
    
    // Start receiving UDP packets
    int err = uv_udp_recv_start(&kcp_data->udp_handle, kcp_alloc_cb, kcp_udp_recv_cb);
    if (err != 0) {
        log_error("uv_udp_recv_start failed: %s", uv_strerror(err));
        return err;
    }
    
    handle->state = TURBO_CONNECTED; // Server is "connected" when listening
    log_debug("KCP listening (UDP server mode)");
    
    // For KCP server, we auto-create connections when packets arrive
    // Trigger the connection callback immediately to indicate server is ready
    if (handle->connect_cb) {
        handle->connect_cb(handle, 0);
    }
    
    return 0;
}

static int kcp_accept(turbo_handle_t* server, turbo_handle_t* client) {
    // For KCP, accept is handled differently - we create new KCP instances
    // when we receive packets from new clients
    UNUSED(server);
    UNUSED(client);
    
    log_warn("KCP accept not implemented - use connection-based handling");
    return UV_ENOTSUP;
}

static int kcp_read_start(turbo_handle_t* handle) {
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (!kcp_data->is_server) {
        // Client mode - start UDP receive
        int err = uv_udp_recv_start(&kcp_data->udp_handle, kcp_alloc_cb, kcp_udp_recv_cb);
        if (err != 0) {
            log_error("uv_udp_recv_start failed: %s", uv_strerror(err));
            return err;
        }
    }
    
    log_debug("KCP read started");
    return 0;
}

static int kcp_read_stop(turbo_handle_t* handle) {
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_udp_recv_stop(&kcp_data->udp_handle);
    if (err != 0) {
        log_error("uv_udp_recv_stop failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("KCP read stopped");
    return 0;
}

static int kcp_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs) {
    turbo_handle_t* handle = req->handle;
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (!kcp_data->kcp) {
        log_error("KCP not initialized for write");
        return UV_EINVAL;
    }
    
    // Send all buffers through KCP
    for (unsigned int i = 0; i < nbufs; i++) {
        int ret = ikcp_send(kcp_data->kcp, bufs[i].base, (int)bufs[i].len);
        if (ret < 0) {
            log_error("ikcp_send failed: %d", ret);
            if (req->write_cb) {
                req->write_cb(req, UV_EIO);
            }
            return UV_EIO;
        }
    }
    
    // Update KCP to flush pending data
    ikcp_update(kcp_data->kcp, kcp_get_current_time());
    
    log_debug("KCP write queued for %d buffers", nbufs);
    
    // KCP write is always "successful" - actual sending happens in output callback
    if (req->write_cb) {
        req->write_cb(req, 0);
    }
    
    return 0;
}

static int kcp_close(turbo_handle_t* handle) {
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Stop timer
    if (!uv_is_closing((uv_handle_t*)&kcp_data->update_timer)) {
        uv_timer_stop(&kcp_data->update_timer);
        uv_close((uv_handle_t*)&kcp_data->update_timer, NULL);
    }
    
    // Close UDP handle
    if (!uv_is_closing((uv_handle_t*)&kcp_data->udp_handle)) {
        uv_close((uv_handle_t*)&kcp_data->udp_handle, kcp_close_cb);
    }
    
    log_debug("KCP close initiated");
    return 0;
}

static int kcp_cleanup(turbo_handle_t* handle) {
    turbo_internal_t* internal = turbo_get_internal(handle);
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)internal->transport_data;
    
    if (kcp_data) {
        // Release KCP instance
        if (kcp_data->kcp) {
            ikcp_release(kcp_data->kcp);
            kcp_data->kcp = NULL;
        }
        
        free(kcp_data);
        internal->transport_data = NULL;
    }
    
    log_debug("KCP cleanup completed");
    return 0;
}

// =============================================================================
// KCP callbacks - bridge between KCP and UDP
// =============================================================================

// KCP output callback - called when KCP wants to send data
static int kcp_output_cb(const char *buf, int len, ikcpcb *kcp, void *user) {
    turbo_handle_t* handle = (turbo_handle_t*)user;
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    UNUSED(kcp);
    
    // Allocate UDP send request
    uv_udp_send_t* send_req = malloc(sizeof(uv_udp_send_t));
    if (!send_req) {
        log_error("Failed to allocate UDP send request");
        return -1;
    }
    
    // Copy data to send buffer (KCP buffer might be reused)
    char* send_data = malloc(len);
    if (!send_data) {
        log_error("Failed to allocate send buffer");
        free(send_req);
        return -1;
    }
    memcpy(send_data, buf, len);
    
    send_req->data = send_data; // Store for cleanup in callback
    
    uv_buf_t send_buf = uv_buf_init(send_data, len);
    
    int err = uv_udp_send(send_req, &kcp_data->udp_handle, &send_buf, 1,
                         (const struct sockaddr*)&kcp_data->remote_addr, 
                         kcp_udp_send_cb);
    
    if (err != 0) {
        log_error("uv_udp_send failed: %s", uv_strerror(err));
        free(send_data);
        free(send_req);
        return -1;
    }
    
    return 0; // Success
}

// KCP update timer - drives KCP state machine
static void kcp_update_timer_cb(uv_timer_t* timer) {
    turbo_handle_t* handle = (turbo_handle_t*)timer->data;
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (kcp_data->kcp) {
        ikcp_update(kcp_data->kcp, kcp_get_current_time());
        
        // Check for incoming data
        int recv_len;
        while ((recv_len = ikcp_recv(kcp_data->kcp, kcp_data->recv_buffer, 
                                    sizeof(kcp_data->recv_buffer))) > 0) {
            
            // Trigger read callback with received data
            if (handle->read_cb) {
                turbo_buf_t recv_buf;
                recv_buf.base = kcp_data->recv_buffer;
                recv_buf.len = recv_len;
                
                handle->bytes_read += recv_len;
                handle->read_cb(handle, recv_len, &recv_buf);
            }
        }
    }
}

// =============================================================================
// libuv callbacks - bridge between UDP and KCP
// =============================================================================

static void kcp_udp_recv_cb(uv_udp_t* udp_handle, ssize_t nread, const uv_buf_t* buf,
                           const struct sockaddr* addr, unsigned flags) {
    turbo_handle_t* handle = (turbo_handle_t*)udp_handle->data;
    turbo_kcp_data_t* kcp_data = (turbo_kcp_data_t*)turbo_get_internal(handle)->transport_data;
    UNUSED(flags);
    
    if (nread > 0) {
        if (kcp_data->is_server && !kcp_data->kcp) {
            // Server mode - create KCP instance on first packet
            // Extract conversation ID from packet (first 4 bytes)
            if (nread >= 4) {
                IUINT32 conv = *(IUINT32*)buf->base;
                
                kcp_data->kcp = ikcp_create(conv, handle);
                if (kcp_data->kcp) {
                    ikcp_setoutput(kcp_data->kcp, kcp_output_cb);
                    ikcp_nodelay(kcp_data->kcp, 1, 10, 2, 1);
                    ikcp_wndsize(kcp_data->kcp, 128, 128);
                    
                    // Store client address
                    memcpy(&kcp_data->remote_addr, addr, 
                          addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6));
                    kcp_data->remote_addrlen = addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
                    
                    // Start update timer for this connection
                    uv_timer_start(&kcp_data->update_timer, kcp_update_timer_cb,
                                  kcp_data->update_interval, kcp_data->update_interval);
                    
                    kcp_data->connected = true;
                    handle->state = TURBO_CONNECTED;
                    
                    // Trigger connection callback for server
                    if (handle->connect_cb) {
                        handle->connect_cb(handle, 0);
                    }
                    
                    log_debug("KCP server accepted connection (conv=%08x)", conv);
                }
            }
        }
        
        if (kcp_data->kcp) {
            // Feed data to KCP
            int ret = ikcp_input(kcp_data->kcp, buf->base, (long)nread);
            if (ret < 0) {
                log_error("ikcp_input failed: %d", ret);
            }
        }
    } else if (nread < 0) {
        log_debug("KCP UDP receive error: %s", uv_strerror((int)nread));
    }
    
    // Always free the buffer
    if (buf->base) {
        free(buf->base);
    }
}

static void kcp_udp_send_cb(uv_udp_send_t* req, int status) {
    char* send_data = (char*)req->data;
    
    if (status != 0) {
        log_debug("KCP UDP send failed: %s", uv_strerror(status));
    }
    
    // Cleanup
    free(send_data);
    free(req);
}

static void kcp_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    UNUSED(handle);
    buf->base = malloc(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}

static void kcp_close_cb(uv_handle_t* handle) {
    turbo_handle_t* turbo_handle = (turbo_handle_t*)((uv_udp_t*)handle)->data;
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
    log_debug("KCP UDP handle closed and cleaned up");
}