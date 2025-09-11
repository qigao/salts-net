/**
 * TLS Transport Implementation
 * "Security should be simple, not scary" - Linus
 * Built on top of TCP + OpenSSL
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "platform.h"
#include "log.h"
#include <uv.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <stdlib.h>
#include <string.h>

// TLS transport data
typedef struct turbo_tls_data_s {
    // Underlying TCP transport
    uv_tcp_t uv_handle;
    uv_connect_t connect_req;
    
    // SSL context and connection
    SSL_CTX* ssl_ctx;
    SSL* ssl;
    BIO* read_bio;  // Input BIO (for SSL_read)
    BIO* write_bio; // Output BIO (for SSL_write)
    
    // TLS state - simplified to single enum
    enum {
        TLS_STATE_INIT = 0,
        TLS_STATE_HANDSHAKING,
        TLS_STATE_READY,
        TLS_STATE_ERROR
    } state;
    
    bool is_server;
    bool reading_started;  // Track if uv_read_start called
    
    // Buffers for SSL I/O
    char* read_buffer;
    size_t read_buffer_size;
    char* write_buffer;
    size_t write_buffer_size;
    
    // Certificate files
    char* cert_file;
    char* key_file;
    char* ca_file;
    char* cipher_list;  // Custom cipher list
    int verify_peer;
} turbo_tls_data_t;

// Forward declarations
static int tls_init(turbo_handle_t* handle);
static int tls_connect(turbo_handle_t* handle, const char* address, int port);
static int tls_bind(turbo_handle_t* handle, const char* address, int port);
static int tls_listen(turbo_handle_t* handle, int backlog);
static int tls_accept(turbo_handle_t* server, turbo_handle_t* client);
static int tls_read_start(turbo_handle_t* handle);
static int tls_read_stop(turbo_handle_t* handle);
static int tls_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
static int tls_close(turbo_handle_t* handle);
static int tls_cleanup(turbo_handle_t* handle);
static int tls_set_option(turbo_handle_t* handle, const char* key, const void* value, size_t len);

// TLS utilities
static int tls_init_ssl_ctx(turbo_tls_data_t* tls_data, bool is_server);
static int tls_load_certificates(turbo_tls_data_t* tls_data);
static int tls_perform_handshake(turbo_handle_t* handle);
static int tls_process_ssl_data(turbo_handle_t* handle);
static void tls_cleanup_ssl_objects(turbo_tls_data_t* tls_data);
static void tls_tcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

// Handshake write callback
static void tls_handshake_write_cb(uv_write_t* uv_req, int status) {
    // The buffer pointer is stored in the request's data field as a struct
    typedef struct { turbo_handle_t* handle; char* buffer; } handshake_req_data_t;
    handshake_req_data_t* req_data = (handshake_req_data_t*)uv_req->data;
    
    // Free the buffer
    if (req_data->buffer) {
        free(req_data->buffer);
    }
    
    if (status != 0) {
        log_error("TLS handshake data send failed: %s", uv_strerror(status));
        req_data->handle->state = TURBO_ERROR;
    }
    
    free(req_data);
    free(uv_req);
}

// Callback for TLS encrypted data write
static void tls_encrypted_write_cb(uv_write_t* uv_req, int status) {
    typedef struct { turbo_req_t* turbo_req; char* buffer; } write_req_data_t;
    write_req_data_t* req_data = (write_req_data_t*)uv_req->data;
    
    log_info("TLS encrypted write callback: status=%d", status);
    
    // Free the persistent buffer
    if (req_data->buffer) {
        free(req_data->buffer);
    }
    
    if (req_data->turbo_req->write_cb) {
        req_data->turbo_req->write_cb(req_data->turbo_req, status);
    }
    
    // Cleanup
    req_data->turbo_req->internal = NULL;
    free(req_data);
    free(uv_req);
}

static void tls_alloc_cb(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = malloc(suggested_size);
    buf->len = buf->base ? suggested_size : 0;
}
static int tls_perform_handshake(turbo_handle_t* handle);
static void tls_tcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

static void tls_close_cb(uv_handle_t* handle);

// libuv callbacks (reuse TCP callbacks where possible)
static void tls_connect_cb(uv_connect_t* req, int status);
static void tls_connection_cb(uv_stream_t* server, int status);

// Transport vtable
static const turbo_transport_vtable_t tls_vtable = {
    .init = tls_init,
    .connect = tls_connect,
    .bind = tls_bind,
    .listen = tls_listen,
    .accept = tls_accept,
    .read_start = tls_read_start,
    .read_stop = tls_read_stop,
    .write = tls_write,
    .close = tls_close,
    .cleanup = tls_cleanup,
    .set_option = tls_set_option,
    .get_option = NULL
};

// Global SSL initialization
static bool g_ssl_initialized = false;

// =============================================================================
// Transport registration
// =============================================================================

void turbo_register_tls(void) {
    if (!g_ssl_initialized) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        g_ssl_initialized = true;
        log_debug("OpenSSL initialized");
    }
    
    turbo_register_transport(TURBO_TLS, &tls_vtable);
}

// =============================================================================
// Transport implementation
// =============================================================================

static int tls_init(turbo_handle_t* handle) {
    turbo_tls_data_t* tls_data = malloc(sizeof(turbo_tls_data_t));
    if (tls_data == NULL) {
        return UV_ENOMEM;
    }
    
    memset(tls_data, 0, sizeof(*tls_data));
    
    // Initialize underlying TCP handle
    int err = uv_tcp_init(handle->loop, &tls_data->uv_handle);
    if (err != 0) {
        log_error("uv_tcp_init failed: %s", uv_strerror(err));
        free(tls_data);
        return err;
    }
    
    tls_data->uv_handle.data = handle;
    
    // Default settings - ENABLE peer verification by default
    tls_data->verify_peer = 1;  // Secure by default
    tls_data->read_buffer_size = 16384;  // 16KB
    tls_data->write_buffer_size = 16384;
    tls_data->state = TLS_STATE_INIT;  // Initialize state machine
    
    turbo_internal_t* internal = turbo_get_internal(handle);
    internal->transport_data = tls_data;
    
    log_debug("TLS transport initialized");
    return 0;
}

static int tls_connect(turbo_handle_t* handle, const char* address, int port) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Initialize SSL context for client
    int err = tls_init_ssl_ctx(tls_data, false);
    if (err != 0) {
        return err;
    }
    
    // Create SSL connection
    tls_data->ssl = SSL_new(tls_data->ssl_ctx);
    if (tls_data->ssl == NULL) {
        log_error("SSL_new failed");
        return UV_ENOMEM;
    }
    
    // Set up separate BIOs for non-blocking I/O
    tls_data->read_bio = BIO_new(BIO_s_mem());
    tls_data->write_bio = BIO_new(BIO_s_mem());
    if (tls_data->read_bio == NULL || tls_data->write_bio == NULL) {
        log_error("BIO_new failed");
        if (tls_data->read_bio) BIO_free(tls_data->read_bio);
        if (tls_data->write_bio) BIO_free(tls_data->write_bio);
        SSL_free(tls_data->ssl);
        return UV_ENOMEM;
    }
    
    SSL_set_bio(tls_data->ssl, tls_data->read_bio, tls_data->write_bio);
    SSL_set_connect_state(tls_data->ssl);
    
    // Apply verification settings to the new SSL object
    int verify_mode = tls_data->verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_set_verify(tls_data->ssl, verify_mode, NULL);
    log_debug("Applied SSL verify mode: %d (verify_peer=%d)", verify_mode, tls_data->verify_peer);
    
    // Set state to INIT - handshake will start when TCP connects
    tls_data->state = TLS_STATE_INIT;
    
    // Allocate I/O buffers
    tls_data->read_buffer = malloc(tls_data->read_buffer_size);
    tls_data->write_buffer = malloc(tls_data->write_buffer_size);
    if (!tls_data->read_buffer || !tls_data->write_buffer) {
        log_error("Failed to allocate TLS I/O buffers");
        tls_cleanup_ssl_objects(tls_data);  // Clean up SSL objects before returning
        return UV_ENOMEM;
    }
    
    // Start TCP connection first - use unified address parsing
    struct sockaddr_storage addr;
    err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    tls_data->connect_req.data = handle;
    err = uv_tcp_connect(&tls_data->connect_req, &tls_data->uv_handle,
                        (const struct sockaddr*)&addr, tls_connect_cb);
    if (err != 0) {
        log_error("uv_tcp_connect failed: %s", uv_strerror(err));
        return err;
    }
    
    log_debug("TLS connect initiated to %s:%d", address, port);
    return 0;
}

static int tls_bind(turbo_handle_t* handle, const char* address, int port) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Initialize SSL context for server (without loading certificates yet)
    int err = tls_init_ssl_ctx(tls_data, true);
    if (err != 0) {
        return err;
    }
    
    // NOTE: Certificate loading is deferred until tls_listen() 
    // This allows the application to set certificates after bind but before listen
    
    // Bind TCP socket - use unified address parsing
    struct sockaddr_storage addr;
    err = turbo_parse_address(address, port, &addr);
    if (err != 0) {
        log_error("Invalid address %s:%d: %s", address, port, uv_strerror(err));
        return err;
    }
    
    err = uv_tcp_bind(&tls_data->uv_handle, (const struct sockaddr*)&addr, 0);
    if (err != 0) {
        log_error("uv_tcp_bind failed: %s", uv_strerror(err));
        return err;
    }
    
    tls_data->is_server = true;
    log_debug("TLS bound to %s:%d", address, port);
    return 0;
}

static int tls_listen(turbo_handle_t* handle, int backlog) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Load certificates now (after bind, before listen) - this is the right time!
    int err = tls_load_certificates(tls_data);
    if (err != 0) {
        log_error("Cannot start TLS server without valid certificates");
        return err;
    }
    
    err = uv_listen((uv_stream_t*)&tls_data->uv_handle, backlog, tls_connection_cb);
    if (err != 0) {
        log_error("uv_listen failed: %s", uv_strerror(err));
        return err;
    }
    
    handle->state = TURBO_CONNECTED;
    log_debug("TLS listening with backlog %d", backlog);
    return 0;
}

static int tls_accept(turbo_handle_t* server, turbo_handle_t* client) {
    // This is more complex for TLS as we need to perform handshake
    turbo_tls_data_t* server_data = (turbo_tls_data_t*)turbo_get_internal(server)->transport_data;
    turbo_tls_data_t* client_data = (turbo_tls_data_t*)turbo_get_internal(client)->transport_data;
    
    // Accept TCP connection first
    int err = uv_accept((uv_stream_t*)&server_data->uv_handle, 
                       (uv_stream_t*)&client_data->uv_handle);
    if (err != 0) {
        log_error("uv_accept failed: %s", uv_strerror(err));
        return err;
    }
    
    // Copy SSL context from server to client
    client_data->ssl_ctx = server_data->ssl_ctx;
    client_data->is_server = true;
    
    // Set up SSL for accepted client
    client_data->ssl = SSL_new(server_data->ssl_ctx);
    if (client_data->ssl == NULL) {
        log_error("SSL_new failed for accepted client");
        return UV_ENOMEM;
    }
    
    // Set up separate BIOs for non-blocking I/O
    client_data->read_bio = BIO_new(BIO_s_mem());
    client_data->write_bio = BIO_new(BIO_s_mem());
    if (client_data->read_bio == NULL || client_data->write_bio == NULL) {
        log_error("BIO_new failed for accepted client");
        if (client_data->read_bio) BIO_free(client_data->read_bio);
        if (client_data->write_bio) BIO_free(client_data->write_bio);
        SSL_free(client_data->ssl);
        return UV_ENOMEM;
    }
    
    SSL_set_bio(client_data->ssl, client_data->read_bio, client_data->write_bio);
    SSL_set_accept_state(client_data->ssl);
    
    // Apply verification settings from server to client connection
    int verify_mode = server_data->verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_set_verify(client_data->ssl, verify_mode, NULL);
    log_debug("Applied SSL verify mode to accepted client: %d", verify_mode);
    
    // Allocate I/O buffers
    client_data->read_buffer_size = 16384;
    client_data->write_buffer_size = 16384;
    client_data->read_buffer = malloc(client_data->read_buffer_size);
    client_data->write_buffer = malloc(client_data->write_buffer_size);
    if (!client_data->read_buffer || !client_data->write_buffer) {
        log_error("Failed to allocate TLS I/O buffers for client");
        tls_cleanup_ssl_objects(client_data);  // Clean up SSL objects before returning
        return UV_ENOMEM;
    }
    
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
    
    client->state = TURBO_CONNECTING; // Will be CONNECTED after handshake
    
    // Start reading for handshake data
    err = uv_read_start((uv_stream_t*)&client_data->uv_handle, 
                       tls_alloc_cb, tls_tcp_read_cb);
    if (err != 0) {
        log_error("Failed to start reading for TLS server handshake: %s", uv_strerror(err));
        return err;
    }
    
    client_data->reading_started = true;  // Mark as reading started
    
    log_debug("TLS client accepted, handshake pending from %s:%d", 
              client->remote_ip, client->remote_port);
    return 0;
}

static int tls_read_start(turbo_handle_t* handle) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Check if already reading
    if (tls_data->reading_started) {
        log_debug("TLS read already started, ignoring");
        return 0;
    }
    
    // Start reading from underlying TCP with proper alloc callback
    int err = uv_read_start((uv_stream_t*)&tls_data->uv_handle, 
                           tls_alloc_cb, tls_tcp_read_cb);
    if (err != 0) {
        log_error("uv_read_start failed: %s", uv_strerror(err));
        return err;
    }
    
    tls_data->reading_started = true;
    log_debug("TLS read started");
    return 0;
}

static int tls_read_stop(turbo_handle_t* handle) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    int err = uv_read_stop((uv_stream_t*)&tls_data->uv_handle);
    if (err != 0) {
        log_error("uv_read_stop failed: %s", uv_strerror(err));
        return err;
    }
    
    tls_data->reading_started = false;
    log_debug("TLS read stopped");
    return 0;
}

static int tls_write(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs) {
    turbo_handle_t* handle = req->handle;
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    log_debug("TLS write called: state=%d, nbufs=%d", tls_data->state, nbufs);
    
    if (tls_data->state != TLS_STATE_READY) {
        log_error("TLS not ready for writing (state: %d)", tls_data->state);
        return UV_ENOTCONN;
    }
    
    // For now, just concatenate all buffers and SSL_write
    size_t total_len = 0;
    for (unsigned int i = 0; i < nbufs; i++) {
        total_len += bufs[i].len;
    }
    
    char* combined_buf = malloc(total_len);
    if (combined_buf == NULL) {
        return UV_ENOMEM;
    }
    
    size_t offset = 0;
    for (unsigned int i = 0; i < nbufs; i++) {
        memcpy(combined_buf + offset, bufs[i].base, bufs[i].len);
        offset += bufs[i].len;
    }
    
    log_info("TLS_write called: %d buffers, total %zu bytes, state=%d", 
             nbufs, total_len, tls_data->state);
    
    // Write to SSL (this encrypts and puts data in BIO)
    int bytes_written = SSL_write(tls_data->ssl, combined_buf, (int)total_len);
    free(combined_buf);
    
    if (bytes_written <= 0) {
        int ssl_error = SSL_get_error(tls_data->ssl, bytes_written);
        log_error("SSL_write failed: SSL_write returned %d, SSL_error %d", bytes_written, ssl_error);
        return UV_EIO;
    }
    
    log_info("SSL_write succeeded: %d bytes written to SSL", bytes_written);
    
    char encrypted_buf[16384];
    int encrypted_bytes = BIO_read(tls_data->write_bio, encrypted_buf, sizeof(encrypted_buf));
    
    log_info("TLS_write: SSL_write returned %d, BIO_read returned %d encrypted bytes", 
             bytes_written, encrypted_bytes);
    
    if (encrypted_bytes <= 0) {
        log_info("No encrypted data from BIO, checking BIO state");
        log_info("BIO pending: %ld, BIO should retry: %d", 
                 BIO_ctrl_pending(tls_data->write_bio), BIO_should_retry(tls_data->write_bio));
    }
    
    if (encrypted_bytes > 0) {
        // Send encrypted data via TCP
        uv_write_t* uv_req = malloc(sizeof(uv_write_t));
        if (uv_req == NULL) {
            return UV_ENOMEM;
        }
        
        // Copy encrypted data to a buffer that will persist
        char* persistent_buf = malloc(encrypted_bytes);
        if (persistent_buf == NULL) {
            free(uv_req);
            return UV_ENOMEM;
        }
        memcpy(persistent_buf, encrypted_buf, encrypted_bytes);
        
        // Create request data structure
        typedef struct { turbo_req_t* turbo_req; char* buffer; } write_req_data_t;
        write_req_data_t* req_data = malloc(sizeof(write_req_data_t));
        if (req_data == NULL) {
            free(persistent_buf);
            free(uv_req);
            return UV_ENOMEM;
        }
        
        req_data->turbo_req = req;
        req_data->buffer = persistent_buf;
        uv_req->data = req_data;
        
        uv_buf_t uv_buf;
        uv_buf.base = persistent_buf;
        uv_buf.len = encrypted_bytes;
        
        req->internal = uv_req; // Store for cleanup
        
        int err = uv_write(uv_req, (uv_stream_t*)&tls_data->uv_handle,
                          &uv_buf, 1, tls_encrypted_write_cb);
        
        if (err != 0) {
            log_error("uv_write failed: %s", uv_strerror(err));
            free(req_data);
            free(persistent_buf);
            free(uv_req);
            return err;
        } else {
            log_info("TLS_write: uv_write initiated for %d encrypted bytes", encrypted_bytes);
        }
        
        log_debug("TLS write: encrypted %d bytes to %d bytes", bytes_written, encrypted_bytes);
    } else {
        // No encrypted data to send, just call callback
        if (req->write_cb) {
            req->write_cb(req, 0);
        }
    }
    
    return 0;
}

static int tls_close(turbo_handle_t* handle) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Shutdown SSL connection
    if (tls_data->ssl) {
        SSL_shutdown(tls_data->ssl);
    }
    
    // Close underlying TCP
    uv_close((uv_handle_t*)&tls_data->uv_handle, tls_close_cb);
    
    log_debug("TLS close initiated");
    return 0;
}

static int tls_cleanup(turbo_handle_t* handle) {
    turbo_internal_t* internal = turbo_get_internal(handle);
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)internal->transport_data;
    
    if (tls_data) {
        // Use unified cleanup for SSL objects and buffers
        tls_cleanup_ssl_objects(tls_data);
        
        // Clean up SSL context
        if (tls_data->ssl_ctx) {
            SSL_CTX_free(tls_data->ssl_ctx);
            tls_data->ssl_ctx = NULL;
        }
        
        // Clean up certificate file paths
        if (tls_data->cert_file) {
            free(tls_data->cert_file);
            tls_data->cert_file = NULL;
        }
        if (tls_data->key_file) {
            free(tls_data->key_file);
            tls_data->key_file = NULL;
        }
        if (tls_data->ca_file) {
            free(tls_data->ca_file);
            tls_data->ca_file = NULL;
        }
        if (tls_data->cipher_list) {
            free(tls_data->cipher_list);
            tls_data->cipher_list = NULL;
        }
        
        free(tls_data);
        internal->transport_data = NULL;
    }
    
    log_debug("TLS cleanup completed");
    return 0;
}

static int tls_set_option(turbo_handle_t* handle, const char* key, const void* value, size_t len) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    if (strcmp(key, "cert_file") == 0) {
        if (tls_data->cert_file) free(tls_data->cert_file);
        tls_data->cert_file = malloc(len + 1);
        if (tls_data->cert_file) {
            memcpy(tls_data->cert_file, value, len);
            tls_data->cert_file[len] = '\0';
        }
        return 0;
    } else if (strcmp(key, "key_file") == 0) {
        if (tls_data->key_file) free(tls_data->key_file);
        tls_data->key_file = malloc(len + 1);
        if (tls_data->key_file) {
            memcpy(tls_data->key_file, value, len);
            tls_data->key_file[len] = '\0';
        }
        return 0;
    } else if (strcmp(key, "ca_file") == 0) {
        if (tls_data->ca_file) free(tls_data->ca_file);
        tls_data->ca_file = malloc(len + 1);
        if (tls_data->ca_file) {
            memcpy(tls_data->ca_file, value, len);
            tls_data->ca_file[len] = '\0';
        }
        return 0;
    } else if (strcmp(key, "verify_peer") == 0) {
        tls_data->verify_peer = *(const int*)value;
        log_debug("Setting verify_peer to: %d", tls_data->verify_peer);
        
        int mode = tls_data->verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;

        // Apply to the live SSL object if it exists
        if (tls_data->ssl) {
            SSL_set_verify(tls_data->ssl, mode, NULL);
            log_debug("Updated live SSL verify mode to: %d", mode);
        }

        // Also apply to the context for future SSL objects
        if (tls_data->ssl_ctx) {
            SSL_CTX_set_verify(tls_data->ssl_ctx, mode, NULL);
            log_debug("Updated SSL_CTX verify mode to: %d", mode);
        }
        return 0;
    } else if (strcmp(key, "cipher_list") == 0) {
        if (tls_data->cipher_list) free(tls_data->cipher_list);
        tls_data->cipher_list = malloc(len + 1);
        if (tls_data->cipher_list) {
            memcpy(tls_data->cipher_list, value, len);
            tls_data->cipher_list[len] = '\0';
        }
        
        // If SSL_CTX already exists, update cipher list immediately
        if (tls_data->ssl_ctx) {
            const char* new_cipher_list = tls_data->cipher_list;
            if (SSL_CTX_set_cipher_list(tls_data->ssl_ctx, new_cipher_list) != 1) {
                log_warn("Failed to update cipher list: %s", new_cipher_list);
            } else {
                log_debug("Updated SSL_CTX cipher list: %s", new_cipher_list);
            }
        }
        
        return 0;
    }
    
    return TURBO_EUNSUPPORTED;
}

// =============================================================================
// TLS utilities
// =============================================================================

// Unified certificate loading function - no more timing dependencies!
static int tls_load_certificates(turbo_tls_data_t* tls_data) {
    if (!tls_data->ssl_ctx) {
        log_error("SSL_CTX not initialized");
        return UV_EINVAL;
    }
    
    // Server must have both cert and key
    if (tls_data->is_server) {
        if (!tls_data->cert_file || !tls_data->key_file) {
            log_error("TLS server requires both certificate and private key files");
            return UV_EINVAL;
        }
        
        log_debug("Loading server certificate: %s", tls_data->cert_file);
        if (SSL_CTX_use_certificate_file(tls_data->ssl_ctx, tls_data->cert_file, SSL_FILETYPE_PEM) != 1) {
            log_error("Failed to load certificate file: %s", tls_data->cert_file);
            return UV_ENOENT;
        }
        
        log_debug("Loading server private key: %s", tls_data->key_file);
        if (SSL_CTX_use_PrivateKey_file(tls_data->ssl_ctx, tls_data->key_file, SSL_FILETYPE_PEM) != 1) {
            log_error("Failed to load private key file: %s", tls_data->key_file);
            return UV_ENOENT;
        }
        
        if (SSL_CTX_check_private_key(tls_data->ssl_ctx) != 1) {
            log_error("Private key does not match certificate");
            return UV_EINVAL;
        }
        
        log_debug("Certificate and private key loaded successfully");
    }
    
    // Client CA verification (optional)
    if (!tls_data->is_server && tls_data->verify_peer && tls_data->ca_file) {
        log_debug("Loading CA file for client verification: %s", tls_data->ca_file);
        if (SSL_CTX_load_verify_locations(tls_data->ssl_ctx, tls_data->ca_file, NULL) != 1) {
            log_error("Failed to load CA file: %s", tls_data->ca_file);
            return UV_ENOENT;
        }
    }
    
    return 0;
}

// Unified SSL data processing - no more if/else branches based on handshake state!
static int tls_process_ssl_data(turbo_handle_t* handle) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    int err =0;
    // State machine-driven processing
    switch (tls_data->state) {
        case TLS_STATE_INIT:
            log_debug("TLS state INIT - starting handshake");
            tls_data->state = TLS_STATE_HANDSHAKING;
            err = tls_perform_handshake(handle);
            log_debug("tls_perform_handshake returned: %d, current state: %d", err, tls_data->state);
            
            // Check if handshake completed immediately (good taste: handle all cases)
            if (err == 0 && tls_data->state == TLS_STATE_READY) {
                handle->state = TURBO_CONNECTED;
                log_info("TLS handshake completed immediately - calling connect callback");
                
                // Call connect callback for successful handshake (CLIENT ONLY)
                if (!tls_data->is_server && handle->connect_cb) {
                    log_debug("Calling connect callback after immediate TLS handshake success");
                    handle->connect_cb(handle, 0);
                } else if (tls_data->is_server) {
                    log_debug("TLS server handshake completed, ready for application data");
                    // After server handshake completes, try to read any pending application data
                    // This mimics uvtls behavior where handshake completion triggers data processing
                    if (handle->read_cb) {
                        log_debug("Server has read callback set, attempting SSL_read for pending data");
                        // Directly try SSL_read since we're now in READY state
                        turbo_buf_t app_buf = { .base = NULL, .len = 0 };
                        if (handle->alloc_cb) {
                            handle->alloc_cb(handle, 16384, &app_buf);
                            if (app_buf.base && app_buf.len > 0) {
                                int decrypted_bytes = SSL_read(tls_data->ssl, app_buf.base, (int)app_buf.len);
                                if (decrypted_bytes > 0) {
                                    handle->bytes_read += decrypted_bytes;
                                    log_debug("Found pending application data: %d bytes", decrypted_bytes);
                                    handle->read_cb(handle, decrypted_bytes, &app_buf);
                                } else {
                                    free(app_buf.base);  // No data, free buffer
                                }
                            }
                        }
                    }
                } else {
                    log_error("No connect callback set for client - this is a bug!");
                }
            }
            return err;
            
        case TLS_STATE_HANDSHAKING: {
            log_debug("TLS_STATE_HANDSHAKING: Processing handshake data");
            err = tls_perform_handshake(handle);
            log_debug("tls_perform_handshake returned: %d, current state: %d", err, tls_data->state);
            if (err == 0) {
                // Check if handshake completed (state updated by tls_perform_handshake)
                if (tls_data->state == TLS_STATE_READY) {
                    handle->state = TURBO_CONNECTED;
                    log_info("TLS handshake completed successfully - calling connect callback");
                    
                    // Call connect callback for successful handshake (CLIENT ONLY)
                    if (!tls_data->is_server && handle->connect_cb) {
                        log_debug("Calling connect callback after TLS handshake success");
                        handle->connect_cb(handle, 0);
                    } else if (tls_data->is_server) {
                        log_debug("TLS server handshake completed, ready for application data");
                        // After server handshake completes, try to process any pending application data
                        // This mimics uvtls behavior where handshake completion triggers data processing
                        if (handle->read_cb) {
                            log_debug("Server has read callback set, attempting SSL_read for pending data");
                            // Directly try SSL_read since we're now in READY state
                            turbo_buf_t app_buf = { .base = NULL, .len = 0 };
                            if (handle->alloc_cb) {
                                handle->alloc_cb(handle, 16384, &app_buf);
                                if (app_buf.base && app_buf.len > 0) {
                                    int decrypted_bytes = SSL_read(tls_data->ssl, app_buf.base, (int)app_buf.len);
                                    if (decrypted_bytes > 0) {
                                        handle->bytes_read += decrypted_bytes;
                                        log_debug("Found pending application data: %d bytes", decrypted_bytes);
                                        handle->read_cb(handle, decrypted_bytes, &app_buf);
                                    } else {
                                        free(app_buf.base);  // No data, free buffer
                                    }
                                }
                            }
                        }
                    } else {
                        log_error("No connect callback set for client - this is a bug!");
                    }
                    
                    // No need for recursive call - normal data processing will handle any pending data
                    return 0;
                } else {
                    // Handshake still in progress
                    log_debug("TLS handshake still in progress, state=%d", tls_data->state);
                    return 0;
                }
            }
            return err;
        }
        
        case TLS_STATE_READY: {
            // Process all available decrypted data in a loop (exactly like uvtls do_read)
            log_debug("TLS_STATE_READY: Processing application data");
            
            while (handle->read_cb) {  // Keep reading while callback is set
                // Allocate buffer using application's alloc callback (like uvtls)
                turbo_buf_t app_buf = { .base = NULL, .len = 0 };
                if (handle->alloc_cb) {
                    handle->alloc_cb(handle, 16384, &app_buf);  // Use suggested size like uvtls
                    if (app_buf.base == NULL || app_buf.len == 0) {
                        log_error("Application alloc callback failed");
                        handle->read_cb(handle, UV_ENOBUFS, &app_buf);
                        return UV_ENOBUFS;
                    }
                } else {
                    log_error("No alloc callback set - this shouldn't happen");
                    return UV_EINVAL;
                }
                
                int decrypted_bytes = SSL_read(tls_data->ssl, app_buf.base, (int)app_buf.len);
                
                if (decrypted_bytes > 0) {
                    handle->bytes_read += decrypted_bytes;
                    log_debug("TLS decrypted %d bytes, calling read callback", decrypted_bytes);
                    
                    // Call read callback with actual bytes read (like uvtls)
                    handle->read_cb(handle, decrypted_bytes, &app_buf);
                    
                    // Reset buffer for next iteration (like uvtls: *buf = uv_buf_init(NULL, 0))
                    app_buf = (turbo_buf_t){ .base = NULL, .len = 0 };
                    
                    // Continue loop to read more data if available
                } else if (decrypted_bytes == 0) {
                    // No more data available - SSL connection might be closed
                    log_debug("SSL_read returned 0, connection closed or no more data");
                    handle->read_cb(handle, UV_EOF, &app_buf);
                    break;
                } else {
                    // decrypted_bytes < 0, check SSL error (like uvtls)
                    int ssl_error = SSL_get_error(tls_data->ssl, decrypted_bytes);
                    if (ssl_error == SSL_ERROR_WANT_READ) {
                        log_debug("SSL_read needs more data (WANT_READ)");
                        // Free the unused buffer and wait for more TCP data
                        if (app_buf.base) {
                            free(app_buf.base);
                        }
                        break;  // Wait for more TCP data
                    } else {
                        // Real SSL error (like uvtls: error == SSL_ERROR_ZERO_RETURN ? UV_EOF : UVTLS_EREAD)
                        log_error("SSL_read failed: SSL error %d", ssl_error);
                        tls_data->state = TLS_STATE_ERROR;
                        int uv_error = (ssl_error == SSL_ERROR_ZERO_RETURN) ? UV_EOF : UV_ECONNRESET;
                        handle->read_cb(handle, uv_error, &app_buf);
                        return uv_error;
                    }
                }
            }
            return 0;
        }
        
        case TLS_STATE_ERROR:
            log_error("TLS in error state, cannot process data");
            return UV_ECONNABORTED;
            
        default:
            log_error("Invalid TLS state: %d", tls_data->state);
            tls_data->state = TLS_STATE_ERROR;
            return UV_EINVAL;
    }
}

// Unified SSL objects cleanup - no more memory leaks!
static void tls_cleanup_ssl_objects(turbo_tls_data_t* tls_data) {
    if (!tls_data) return;
    
    // Clean up SSL objects in correct order
    if (tls_data->ssl) {
        SSL_free(tls_data->ssl);  // This also frees the BIOs
        tls_data->ssl = NULL;
        tls_data->read_bio = NULL;  // Don't double-free
        tls_data->write_bio = NULL;
    } else {
        // If SSL object wasn't created, clean BIOs separately
        if (tls_data->read_bio) {
            BIO_free(tls_data->read_bio);
            tls_data->read_bio = NULL;
        }
        if (tls_data->write_bio) {
            BIO_free(tls_data->write_bio);
            tls_data->write_bio = NULL;
        }
    }
    
    // Clean up I/O buffers
    if (tls_data->read_buffer) {
        free(tls_data->read_buffer);
        tls_data->read_buffer = NULL;
    }
    if (tls_data->write_buffer) {
        free(tls_data->write_buffer);
        tls_data->write_buffer = NULL;
    }
    
    // Reset state
    tls_data->state = TLS_STATE_ERROR;
}

static int tls_init_ssl_ctx(turbo_tls_data_t* tls_data, bool is_server) {
    const SSL_METHOD* method = is_server ? TLS_server_method() : TLS_client_method();
    
    tls_data->ssl_ctx = SSL_CTX_new(method);
    if (tls_data->ssl_ctx == NULL) {
        log_error("SSL_CTX_new failed");
        return UV_ENOMEM;
    }
    
    // Set secure options - be more permissive for debugging  
    SSL_CTX_set_options(tls_data->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
    log_debug("SSL_CTX options set, allowing TLS 1.0+\n");
    log_debug("Allowing TLS 1.0+ (removed TLS 1.0/1.1 restrictions)");
    
    // TEMPORARY DEBUG: Don't set any cipher list, use OpenSSL defaults
    /*
    // Set modern cipher suites - use custom list if provided, otherwise compatible defaults
    const char* cipher_list;
    if (tls_data->cipher_list && strlen(tls_data->cipher_list) > 0) {
        cipher_list = tls_data->cipher_list;  // User-specified cipher list
        log_debug("Using custom cipher list: %s", cipher_list);
    } else {
        // Very permissive default for maximum compatibility - temporary debug
        cipher_list = "ALL:!aNULL:!eNULL:!EXPORT:!DES:!MD5:!PSK:!SRP:!CAMELLIA:!SEED";
        log_debug("Using very permissive cipher list for debugging");
    }
    
    if (SSL_CTX_set_cipher_list(tls_data->ssl_ctx, cipher_list) != 1) {
        // If our list fails, try an even more permissive one
        const char* fallback_list = "HIGH:!aNULL:!MD5:!RC4";
        log_warn("Failed to set cipher list: %s, trying fallback: %s", cipher_list, fallback_list);
        if (SSL_CTX_set_cipher_list(tls_data->ssl_ctx, fallback_list) != 1) {
            log_warn("Failed to set fallback cipher list, using OpenSSL defaults");
        }
    }
    */
    log_debug("Using OpenSSL default cipher list (no custom cipher restrictions)\n");
    log_debug("Using OpenSSL default cipher list");
    
    // TEMPORARY DEBUG: Don't force minimum TLS version
    /*
    // Set minimum TLS version to 1.2 for security
    if (SSL_CTX_set_min_proto_version(tls_data->ssl_ctx, TLS1_2_VERSION) != 1) {
        log_warn("Failed to set minimum TLS version");
    }
    */
    log_debug("No minimum TLS version restriction\n");
    log_debug("Allowing all TLS versions (no minimum version restriction)");
    
    if (is_server) {
        // Server-specific verification mode
        SSL_CTX_set_verify(tls_data->ssl_ctx, SSL_VERIFY_NONE, NULL);
        tls_data->is_server = true;
    } else {
        // Client verification settings
        int mode = tls_data->verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
        SSL_CTX_set_verify(tls_data->ssl_ctx, mode, NULL);
        
        if (tls_data->verify_peer && !tls_data->ca_file) {
            // Use system CA store if no custom CA specified
            SSL_CTX_set_default_verify_paths(tls_data->ssl_ctx);
        }
    }
    
    log_debug("SSL context initialized for %s", is_server ? "server" : "client");
    return 0;
}

// =============================================================================
// Callbacks
// =============================================================================

static void tls_connect_cb(uv_connect_t* req, int status) {
    turbo_handle_t* handle = (turbo_handle_t*)req->data;
    
    if (status == 0) {
        // TCP connected successfully, now start TLS handshake
        log_debug("TCP connection established, starting TLS handshake");
        
        // Start reading from TCP to handle handshake data
        turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
        int err = uv_read_start((uv_stream_t*)&tls_data->uv_handle, 
                               tls_alloc_cb, tls_tcp_read_cb);
        if (err != 0) {
            log_error("Failed to start reading for TLS handshake: %s", uv_strerror(err));
            handle->state = TURBO_ERROR;
            status = err;
        } else {
            tls_data->reading_started = true;  // Mark as reading started
            // Initiate TLS handshake (client side)
            err = tls_perform_handshake(handle);
            if (err != 0) {
                log_error("Failed to initiate TLS handshake: %s", uv_strerror(err));
                handle->state = TURBO_ERROR;
                status = err;
            } else {
                // Don't set CONNECTED yet - wait for handshake completion
                log_debug("TLS handshake initiated successfully");
                return; // Don't call callback yet
            }
        }
    } else {
        handle->state = TURBO_ERROR;
        log_error("TCP connect failed: %s", uv_strerror(status));
    }
    
    // Only call callback if there was an error or immediate completion
    if (handle->connect_cb) {
        handle->connect_cb(handle, status);
    }
}

static void tls_connection_cb(uv_stream_t* server, int status) {
    turbo_handle_t* handle = (turbo_handle_t*)server->data;
    
    log_debug("TLS connection callback: status=%d", status);
    
    if (handle->connect_cb) {
        handle->connect_cb(handle, status);
    }
}

static int tls_perform_handshake(turbo_handle_t* handle) {
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    // Perform non-blocking handshake
    int ret = SSL_do_handshake(tls_data->ssl);
    if (ret == 1) {
        // Handshake completed successfully
        tls_data->state = TLS_STATE_READY;
        log_info("TLS handshake completed successfully");
        return 0;
    } else {
        int ssl_error = SSL_get_error(tls_data->ssl, ret);
        
        // Log detailed SSL error information
        unsigned long err_code = ERR_get_error();
        if (err_code != 0) {
            char err_buf[256];
            ERR_error_string_n(err_code, err_buf, sizeof(err_buf));
            log_error("SSL error details: %s", err_buf);
        }
        
        if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
            // Handshake needs more data - check if we have encrypted data to send
            char handshake_buf[16384];
            int handshake_bytes = BIO_read(tls_data->write_bio, handshake_buf, sizeof(handshake_buf));
            
            if (handshake_bytes > 0) {
                // Send handshake data via TCP
                uv_write_t* uv_req = malloc(sizeof(uv_write_t));
                if (uv_req == NULL) {
                    return UV_ENOMEM;
                }
                
                char* persistent_buf = malloc(handshake_bytes);
                if (persistent_buf == NULL) {
                    free(uv_req);
                    return UV_ENOMEM;
                }
                memcpy(persistent_buf, handshake_buf, handshake_bytes);
                
                // Create request data structure for handshake
                typedef struct { turbo_handle_t* handle; char* buffer; } handshake_req_data_t;
                handshake_req_data_t* req_data = malloc(sizeof(handshake_req_data_t));
                if (req_data == NULL) {
                    free(persistent_buf);
                    free(uv_req);
                    return UV_ENOMEM;
                }
                
                req_data->handle = handle;
                req_data->buffer = persistent_buf;
                uv_req->data = req_data;
                
                uv_buf_t uv_buf;
                uv_buf.base = persistent_buf;
                uv_buf.len = handshake_bytes;
                
                int err = uv_write(uv_req, (uv_stream_t*)&tls_data->uv_handle,
                                  &uv_buf, 1, tls_handshake_write_cb);
                
                if (err != 0) {
                    log_error("Failed to send TLS handshake data: %s", uv_strerror(err));
                    free(req_data);
                    free(persistent_buf);
                    free(uv_req);
                    return err;
                }
                
                log_debug("TLS handshake: sent %d bytes", handshake_bytes);
            }
            
            log_debug("TLS handshake in progress (SSL error %d: %s)", ssl_error, 
                     ssl_error == SSL_ERROR_WANT_READ ? "WANT_READ" : "WANT_WRITE");
            return 0; // Continue handshake
        } else {
            // Get detailed SSL error information
            unsigned long ssl_err_code = ERR_get_error();
            char ssl_context[128];
            snprintf(ssl_context, sizeof(ssl_context), "TLS handshake (SSL error %d)", ssl_error);
            
            // Use the new error handling system
            turbo_set_tls_error(handle, TURBO_ETLS_HANDSHAKE, ssl_context, ssl_err_code);
            
            log_error("TLS handshake failed: SSL error %d (%s)", ssl_error,
                     ssl_error == SSL_ERROR_SSL ? "SSL_ERROR_SSL" :
                     ssl_error == SSL_ERROR_SYSCALL ? "SSL_ERROR_SYSCALL" :
                     ssl_error == SSL_ERROR_ZERO_RETURN ? "SSL_ERROR_ZERO_RETURN" : "OTHER");
            
            // Print OpenSSL error stack
            ERR_print_errors_fp(stderr);
            return UV_ECONNABORTED;
        }
    }
}

static void tls_tcp_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    turbo_handle_t* handle = (turbo_handle_t*)stream->data;
    turbo_tls_data_t* tls_data = (turbo_tls_data_t*)turbo_get_internal(handle)->transport_data;
    
    log_info("TLS TCP read callback: nread=%zd state=%d", nread, tls_data->state);
    
    if (nread > 0) {
        log_debug("TLS TCP read: %zd bytes", nread);
        
        // Feed data to SSL read BIO
        int bio_written = BIO_write(tls_data->read_bio, buf->base, (int)nread);
        if (bio_written != nread) {
            log_error("BIO_write incomplete: wrote %d of %zd bytes", bio_written, nread);
        }
        
        // Process SSL data using unified state machine - no more if/else branches!
        int original_state = tls_data->state;  // Save state before processing
        int err = tls_process_ssl_data(handle);
        if (err != 0 && err != UV_ECONNRESET) {
            log_error("TLS data processing failed: %d", err);
            handle->state = TURBO_ERROR;
            tls_data->state = TLS_STATE_ERROR;
            
            // Call appropriate callback based on original TLS state (before error)
            if (original_state == TLS_STATE_HANDSHAKING && handle->connect_cb) {
                handle->connect_cb(handle, err);
            } else if (handle->read_cb) {
                handle->read_cb(handle, err, NULL);
            }
        }
    } else if (nread < 0) {
        log_error("TLS TCP read error: %s", uv_strerror((int)nread));
        int original_state = tls_data->state;  // Save state before error
        handle->state = TURBO_ERROR;
        tls_data->state = TLS_STATE_ERROR;
        
        // Call appropriate callback based on original TLS state (before error)
        if (original_state == TLS_STATE_HANDSHAKING && handle->connect_cb) {
            handle->connect_cb(handle, (int)nread);
        } else if (handle->read_cb) {
            handle->read_cb(handle, nread, NULL);
        }
    }
    
    if (buf->base) {
        free(buf->base);
    }
}

static void tls_close_cb(uv_handle_t* handle) {
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
    log_debug("TLS handle closed and cleaned up");
}
