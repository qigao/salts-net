/**
 * QUIC Transport Implementation
 * Based on PicoQUIC sample - proper integration with picoquic_packet_loop
 */
#include "turbonet.h"
#include "turbonet_internal.h" 
#include "platform.h"
#include "log.h"
#include <uv.h>
#include <stdlib.h>
#include <string.h>

// PicoQUIC includes
#include <picoquic.h>
#include <picoquic_utils.h>
#include <picosocks.h>
#include <picoquic_packet_loop.h>

#define QUIC_DEFAULT_ALPN "turbonet"
#define QUIC_MAIN_STREAM_ID 4  // Client-initiated bidirectional stream

// QUIC transport data - thread-based approach
typedef struct turbo_quic_data_s {
    // PicoQUIC context and connection
    picoquic_quic_t* quic_ctx;
    picoquic_cnx_t* quic_cnx;
    
    // Configuration
    char* server_name;
    char* alpn;
    int server_port;
    
    // Connection state
    bool is_server;
    bool connected;
    bool reading;
    bool should_stop;
    uint64_t main_stream_id;
    
    // Threading for packet loop integration
    uv_thread_t quic_thread;
    uv_mutex_t state_mutex;
    uv_async_t async_handle;
    
    // Address storage
    struct sockaddr_storage bind_addr;
    struct sockaddr_storage remote_addr;
    socklen_t addr_len;
    
    // Back reference
    turbo_handle_t* handle;
    
    // Write buffer for cross-thread communication
    struct {
        uint8_t* data;
        size_t len;
        bool pending;
    } write_buffer;
} turbo_quic_data_t;

// Forward declarations
static int quic_init(turbo_handle_t* handle);
static int quic_connect(turbo_handle_t* handle, const char* address, int port);
static int quic_bind(turbo_handle_t* handle, const char* address, int port);
static int quic_listen(turbo_handle_t* handle, int backlog);
static int quic_accept(turbo_handle_t* server, turbo_handle_t* client);
static int quic_read_start(turbo_handle_t* handle);
static int quic_read_stop(turbo_handle_t* handle);
static int quic_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
static int quic_close(turbo_handle_t* handle);
static int quic_cleanup(turbo_handle_t* handle);

// PicoQUIC callback
static int quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
                        picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx);

// Thread functions
static void quic_thread_func(void* arg);
static void quic_async_cb(uv_async_t* async);
static int quic_run_client(turbo_quic_data_t* quic_data);
static int quic_run_server(turbo_quic_data_t* quic_data);

// Transport vtable
static const turbo_transport_vtable_t quic_vtable = {
    .init = quic_init,
    .connect = quic_connect,
    .bind = quic_bind,
    .listen = quic_listen,
    .accept = quic_accept,
    .read_start = quic_read_start,
    .read_stop = quic_read_stop,
    .write = quic_write,
    .close = quic_close,
    .cleanup = quic_cleanup,
    .set_option = NULL,
    .get_option = NULL
};

// =============================================================================
// Transport registration
// =============================================================================

void turbo_register_quic(void) {
    turbo_register_transport(TURBO_QUIC, &quic_vtable);
}

// =============================================================================
// Transport implementation
// =============================================================================

static int quic_init(turbo_handle_t* handle) {
    turbo_quic_data_t* quic_data = malloc(sizeof(turbo_quic_data_t));
    if (quic_data == NULL) {
        return UV_ENOMEM;
    }
    
    memset(quic_data, 0, sizeof(*quic_data));
    quic_data->handle = handle;
    quic_data->main_stream_id = QUIC_MAIN_STREAM_ID;
    
    // Initialize mutex
    int err = uv_mutex_init(&quic_data->state_mutex);
    if (err != 0) {
        free(quic_data);
        return err;
    }
    
    // Initialize async handle for cross-thread communication
    err = uv_async_init(handle->loop, &quic_data->async_handle, quic_async_cb);
    if (err != 0) {
        uv_mutex_destroy(&quic_data->state_mutex);
        free(quic_data);
        return err;
    }
    quic_data->async_handle.data = quic_data;
    
    // Default configuration
    quic_data->alpn = turbo_strdup(QUIC_DEFAULT_ALPN);
    quic_data->server_name = turbo_strdup("localhost");
    
    turbo_internal_t* internal = turbo_get_internal(handle);
    internal->transport_data = quic_data;
    
    log_debug("QUIC transport initialized");
    return 0;
}

static int quic_connect(turbo_handle_t* handle, const char* address, int port) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Store connection details
    if (quic_data->server_name) {
        free(quic_data->server_name);
    }
    quic_data->server_name = turbo_strdup(address);
    quic_data->server_port = port;
    quic_data->is_server = false;
    
    // Parse remote address
    struct sockaddr_in* addr = (struct sockaddr_in*)&quic_data->remote_addr;
    memset(addr, 0, sizeof(struct sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    
    if (inet_pton(AF_INET, address, &addr->sin_addr) != 1) {
        log_error("Invalid server address %s:%d", address, port);
        return UV_EINVAL;
    }
    quic_data->addr_len = sizeof(struct sockaddr_in);
    
    // Store address info in handle
    strncpy(handle->remote_ip, address, sizeof(handle->remote_ip) - 1);
    handle->remote_ip[sizeof(handle->remote_ip) - 1] = '\0';
    handle->remote_port = port;
    
    // Start QUIC thread with packet loop
    int err = uv_thread_create(&quic_data->quic_thread, quic_thread_func, quic_data);
    if (err != 0) {
        log_error("Failed to create QUIC thread: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("QUIC connect initiated to %s:%d", address, port);
    return 0;
}

static int quic_bind(turbo_handle_t* handle, const char* address, int port) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Parse bind address
    struct sockaddr_in* addr = (struct sockaddr_in*)&quic_data->bind_addr;
    memset(addr, 0, sizeof(struct sockaddr_in));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    
    if (inet_pton(AF_INET, address, &addr->sin_addr) != 1) {
        log_error("Invalid bind address %s:%d", address, port);
        return UV_EINVAL;
    }
    quic_data->addr_len = sizeof(struct sockaddr_in);
    quic_data->server_port = port;
    quic_data->is_server = true;
    
    // Store address info in handle
    strncpy(handle->local_ip, address, sizeof(handle->local_ip) - 1);
    handle->local_ip[sizeof(handle->local_ip) - 1] = '\0';
    handle->local_port = port;
    
    log_debug("QUIC bound to %s:%d", address, port);
    return 0;
}

static int quic_listen(turbo_handle_t* handle, int backlog) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    UNUSED(backlog);
    
    // Start QUIC server thread with packet loop
    int err = uv_thread_create(&quic_data->quic_thread, quic_thread_func, quic_data);
    if (err != 0) {
        log_error("Failed to create QUIC server thread: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("QUIC server listening");
    return 0;
}

static int quic_accept(turbo_handle_t* server, turbo_handle_t* client) {
    // QUIC connections are handled automatically in callbacks
    UNUSED(server);
    UNUSED(client);
    log_warn("QUIC accept - connections handled automatically in callback");
    return UV_ENOTSUP;
}

static int quic_read_start(turbo_handle_t* handle) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    uv_mutex_lock(&quic_data->state_mutex);
    quic_data->reading = true;
    uv_mutex_unlock(&quic_data->state_mutex);
    
    log_debug("QUIC read started");
    return 0;
}

static int quic_read_stop(turbo_handle_t* handle) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    uv_mutex_lock(&quic_data->state_mutex);
    quic_data->reading = false;
    uv_mutex_unlock(&quic_data->state_mutex);
    
    log_debug("QUIC read stopped");
    return 0;
}

static int quic_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs) {
    turbo_handle_t* handle = req->handle;
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (!quic_data->connected || !quic_data->quic_cnx) {
        log_error("QUIC not connected for write");
        if (req->write_cb) {
            req->write_cb(req, UV_ENOTCONN);
        }
        return UV_ENOTCONN;
    }
    
    // Store write data for the prepare_to_send callback
    if (nbufs > 0) {
        uv_mutex_lock(&quic_data->state_mutex);
        
        if (!quic_data->write_buffer.pending) {
            // Store write data for cross-thread access
            quic_data->write_buffer.len = bufs[0].len;
            quic_data->write_buffer.data = malloc(bufs[0].len);
            if (quic_data->write_buffer.data) {
                memcpy(quic_data->write_buffer.data, bufs[0].base, bufs[0].len);
                quic_data->write_buffer.pending = true;
                handle->bytes_written += bufs[0].len;
                
                log_info("QUIC write: stored %zu bytes, marking stream %lu active", bufs[0].len, (unsigned long)quic_data->main_stream_id);
                
                // Trigger prepare_to_send by marking stream as active again
                int ret = picoquic_mark_active_stream(quic_data->quic_cnx, quic_data->main_stream_id, 1, quic_data);
                if (ret != 0) {
                    log_error("QUIC failed to reactivate stream for write: %d", ret);
                }
            } else {
                log_error("QUIC write: failed to allocate buffer");
                uv_mutex_unlock(&quic_data->state_mutex);
                if (req->write_cb) {
                    req->write_cb(req, UV_ENOMEM);
                }
                return UV_ENOMEM;
            }
        } else {
            log_warn("QUIC write: buffer already pending, dropping write");
        }
        
        uv_mutex_unlock(&quic_data->state_mutex);
    }
    
    log_debug("QUIC write queued for %d buffers", nbufs);
    
    // Call write callback immediately 
    if (req->write_cb) {
        req->write_cb(req, 0);
    }
    
    return 0;
}

static int quic_close(turbo_handle_t* handle) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    uv_mutex_lock(&quic_data->state_mutex);
    quic_data->should_stop = true;
    uv_mutex_unlock(&quic_data->state_mutex);
    
    log_debug("QUIC close initiated");
    return 0;
}

static int quic_cleanup(turbo_handle_t* handle) {
    turbo_internal_t* internal = turbo_get_internal(handle);
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)internal->transport_data;
    
    if (quic_data) {
        // Stop thread
        uv_mutex_lock(&quic_data->state_mutex);
        quic_data->should_stop = true;
        uv_mutex_unlock(&quic_data->state_mutex);
        
        // Wait for thread to finish
        uv_thread_join(&quic_data->quic_thread);
        
        // Cleanup
        if (quic_data->quic_ctx) {
            picoquic_free(quic_data->quic_ctx);
        }
        
        if (quic_data->write_buffer.data) {
            free(quic_data->write_buffer.data);
        }
        
        if (quic_data->server_name) free(quic_data->server_name);
        if (quic_data->alpn) free(quic_data->alpn);
        
        uv_close((uv_handle_t*)&quic_data->async_handle, NULL);
        uv_mutex_destroy(&quic_data->state_mutex);
        
        free(quic_data);
        internal->transport_data = NULL;
    }
    
    log_debug("QUIC cleanup completed");
    return 0;
}

// =============================================================================
// Thread functions - runs PicoQUIC packet loop in separate thread
// =============================================================================

static void quic_thread_func(void* arg) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)arg;
    
    if (quic_data->is_server) {
        quic_run_server(quic_data);
    } else {
        quic_run_client(quic_data);
    }
}

static int quic_run_client(turbo_quic_data_t* quic_data) {
    uint64_t current_time = picoquic_current_time();
    
    log_info("QUIC client thread started");
    
    // Create QUIC context
    quic_data->quic_ctx = picoquic_create(1, NULL, NULL, NULL, quic_data->alpn, NULL, NULL,
                                         NULL, NULL, NULL, current_time, NULL, NULL, NULL, 0);
    if (!quic_data->quic_ctx) {
        log_error("Failed to create QUIC client context");
        return -1;
    }
    
    // Set client configurations (like the sample)
    picoquic_set_log_level(quic_data->quic_ctx, 1);
    
    log_info("QUIC client context created");
    
    // Create connection
    quic_data->quic_cnx = picoquic_create_cnx(quic_data->quic_ctx,
                                             picoquic_null_connection_id,
                                             picoquic_null_connection_id,
                                             (struct sockaddr*)&quic_data->remote_addr,
                                             current_time, 0,
                                             quic_data->server_name, quic_data->alpn, 1);
    
    if (!quic_data->quic_cnx) {
        log_error("Failed to create QUIC connection");
        return -1;
    }
    
    log_info("QUIC connection object created");
    
    // Set callback
    picoquic_set_callback(quic_data->quic_cnx, quic_callback, quic_data);
    
    // Start connection
    int ret = picoquic_start_client_cnx(quic_data->quic_cnx);
    if (ret < 0) {
        log_error("Failed to start QUIC client connection");
        return ret;
    }
    
    // Create and activate the main stream immediately (like the sample does)
    uint64_t stream_id = picoquic_get_next_local_stream_id(quic_data->quic_cnx, 0);
    quic_data->main_stream_id = stream_id;
    
    ret = picoquic_mark_active_stream(quic_data->quic_cnx, stream_id, 1, quic_data);
    if (ret != 0) {
        log_error("Failed to mark stream active: %d", ret);
        return ret;
    }
    
    log_info("QUIC client connection started, stream %lu activated", (unsigned long)stream_id);
    
    log_info("Starting QUIC packet loop...");
    
    // Run packet loop
    ret = picoquic_packet_loop(quic_data->quic_ctx, 0, 
                              quic_data->remote_addr.ss_family, 0, 0, 0, NULL, NULL);
    
    log_info("QUIC client packet loop exited with %d", ret);
    return ret;
}

static int quic_run_server(turbo_quic_data_t* quic_data) {
    uint64_t current_time = picoquic_current_time();
    
    log_info("QUIC server thread started");
    
    // Create QUIC server context with certificates
    const char* server_cert = "server.pem";
    const char* server_key = "server.key"; 
    
    quic_data->quic_ctx = picoquic_create(8, server_cert, server_key, NULL, quic_data->alpn,
                                         quic_callback, quic_data, NULL, NULL, NULL,
                                         current_time, NULL, NULL, NULL, 0);
    if (!quic_data->quic_ctx) {
        log_error("Failed to create QUIC server context");
        return -1;
    }
    
    // Set server configurations (like the sample)
    picoquic_set_log_level(quic_data->quic_ctx, 1);
    
    log_info("QUIC server context created");
    
    // Notify main thread that server is ready
    uv_async_send(&quic_data->async_handle);
    
    log_info("Starting QUIC server packet loop on port %d...", quic_data->server_port);
    
    // Run packet loop
    int ret = picoquic_packet_loop(quic_data->quic_ctx, quic_data->server_port, 0, 0, 0, 0, NULL, NULL);
    
    log_info("QUIC server packet loop exited with %d", ret);
    return ret;
}

// =============================================================================
// PicoQUIC callback - handles QUIC protocol events
// =============================================================================

static int quic_callback(picoquic_cnx_t* cnx, uint64_t stream_id, uint8_t* bytes, size_t length,
                        picoquic_call_back_event_t fin_or_event, void* callback_ctx, void* v_stream_ctx) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)callback_ctx;
    turbo_handle_t* handle = quic_data->handle;
    UNUSED(v_stream_ctx);
    UNUSED(cnx);
    
    // Debug: log all callback events
    const char* event_name = "unknown";
    switch (fin_or_event) {
        case picoquic_callback_stream_data: event_name = "stream_data"; break;
        case picoquic_callback_stream_fin: event_name = "stream_fin"; break;
        case picoquic_callback_ready: event_name = "ready"; break;
        case picoquic_callback_close: event_name = "close"; break;
        case picoquic_callback_application_close: event_name = "application_close"; break;
        case picoquic_callback_prepare_to_send: event_name = "prepare_to_send"; break;
        default: break;
    }
    log_info("QUIC callback: %s, stream=%lu, length=%zu", event_name, (unsigned long)stream_id, length);
    
    switch (fin_or_event) {
        case picoquic_callback_stream_data:
        case picoquic_callback_stream_fin:
            // Handle incoming data - need to handle per-stream like the sample
            if (length > 0) {
                log_info("QUIC received %zu bytes on stream %lu", length, (unsigned long)stream_id);
                
                // Store the stream ID for echo responses (server needs to reply on same stream)
                if (quic_data->is_server) {
                    quic_data->main_stream_id = stream_id;
                    log_info("QUIC server: updated main_stream_id to %lu for echo", (unsigned long)stream_id);
                }
                
                if (quic_data->reading && handle->read_cb) {
                    turbo_buf_t buf;
                    buf.base = (char*)bytes;
                    buf.len = length;
                    
                    handle->bytes_read += length;
                    
                    // Call read callback directly (should move to async for thread safety)
                    handle->read_cb(handle, (ssize_t)length, &buf);
                    log_info("QUIC data passed to read callback");
                } else {
                    log_info("QUIC data received but not reading or no callback");
                }
            }
            break;
            
        case picoquic_callback_ready:
            // Connection established
            uv_mutex_lock(&quic_data->state_mutex);
            if (!quic_data->connected) {
                quic_data->connected = true;
                quic_data->quic_cnx = cnx;
                handle->state = TURBO_CONNECTED;
                handle->connect_time = turbo_hrtime();
                
                log_info("QUIC connection established");
                
                // For server connections, we need to trigger the connection callback
                if (quic_data->is_server && handle->connect_cb) {
                    log_debug("QUIC server: triggering connection callback");
                }
                
                // Notify main thread
                uv_async_send(&quic_data->async_handle);
            }
            uv_mutex_unlock(&quic_data->state_mutex);
            break;
            
        case picoquic_callback_close:
        case picoquic_callback_application_close:
            // Connection closed
            handle->state = TURBO_CLOSED;
            log_debug("QUIC connection closed");
            uv_async_send(&quic_data->async_handle);
            break;
            
        case picoquic_callback_prepare_to_send:
            // Handle outgoing data - check if this is for our main stream
            log_info("QUIC prepare_to_send on stream %lu, available length=%zu", (unsigned long)stream_id, length);
            
            uv_mutex_lock(&quic_data->state_mutex);
            if (quic_data->write_buffer.pending && stream_id == quic_data->main_stream_id) {
                if (length >= quic_data->write_buffer.len) {
                    uint8_t* buffer = picoquic_provide_stream_data_buffer(bytes, quic_data->write_buffer.len, 1, 0);
                    if (buffer) {
                        memcpy(buffer, quic_data->write_buffer.data, quic_data->write_buffer.len);
                        log_info("QUIC data sent: %zu bytes on stream %lu", quic_data->write_buffer.len, (unsigned long)stream_id);
                        
                        free(quic_data->write_buffer.data);
                        quic_data->write_buffer.data = NULL;
                        quic_data->write_buffer.pending = false;
                    } else {
                        log_error("QUIC failed to get stream buffer");
                    }
                } else {
                    log_info("QUIC prepare_to_send: not enough space, need %zu have %zu", quic_data->write_buffer.len, length);
                }
            } else if (quic_data->write_buffer.pending) {
                log_info("QUIC prepare_to_send: wrong stream ID, expected %lu got %lu", (unsigned long)quic_data->main_stream_id, (unsigned long)stream_id);
            }
            uv_mutex_unlock(&quic_data->state_mutex);
            break;
            
        default:
            break;
    }
    
    return 0;
}

// =============================================================================
// Async callback - bridge between QUIC thread and main thread
// =============================================================================

static void quic_async_cb(uv_async_t* async) {
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)async->data;
    turbo_handle_t* handle = quic_data->handle;
    
    uv_mutex_lock(&quic_data->state_mutex);
    
    if (quic_data->connected && handle->state == TURBO_CONNECTED && handle->connect_cb) {
        log_info("QUIC async: triggering connect callback (server=%d)", quic_data->is_server);
        
        if (quic_data->is_server) {
            // For server, this is a new connection - call connection callback  
            handle->connect_cb(handle, 0);
        } else {
            // For client, this is connection established - call connect callback
            handle->connect_cb(handle, 0);
        }
        
        // Clear callback to avoid multiple calls
        handle->connect_cb = NULL;
    } else if (handle->state == TURBO_CLOSED && handle->close_cb) {
        log_debug("QUIC async: triggering close callback");
        handle->close_cb(handle);
    }
    
    uv_mutex_unlock(&quic_data->state_mutex);
}

// =============================================================================
// QUIC-specific configuration functions
// =============================================================================

int turbo_quic_set_alpn(turbo_handle_t* handle, const char* alpn) {
    if (handle->transport != TURBO_QUIC) {
        return UV_EINVAL;
    }
    
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (quic_data->alpn) {
        free(quic_data->alpn);
    }
    quic_data->alpn = turbo_strdup(alpn);
    
    log_debug("QUIC ALPN set to: %s", alpn);
    return 0;
}

int turbo_quic_set_server_name(turbo_handle_t* handle, const char* server_name) {
    if (handle->transport != TURBO_QUIC) {
        return UV_EINVAL;
    }
    
    turbo_quic_data_t* quic_data = (turbo_quic_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (quic_data->server_name) {
        free(quic_data->server_name);
    }
    quic_data->server_name = turbo_strdup(server_name);
    
    log_debug("QUIC server name set to: %s", server_name);
    return 0;
}