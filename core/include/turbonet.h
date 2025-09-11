/**
 * @file turbonet.h
 * @brief TurboNet Unified Transport Layer
 * @author Follows Linux philosophy: "Good taste: eliminate special cases, not add them"
 *
 * One API to rule them all: TCP, TLS, KCP, UDP, PIPE, QUIC
 *
 * This header provides a unified networking API that works across all transport
 * protocols. The goal is to eliminate protocol-specific code duplication while
 * maintaining performance and providing familiar socket-like semantics.
 */

#ifndef TURBONET_H
#define TURBONET_H

#include "platform.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct turbo_handle_s turbo_handle_t;
typedef struct turbo_req_s turbo_req_t;
typedef struct turbo_buf_s turbo_buf_t;
typedef struct turbo_error_s turbo_error_t;
typedef struct uv_loop_s uv_loop_t;  // libuv event loop
typedef struct sockaddr_storage sockaddr_storage;

/**
 * @brief Transport protocol types supported by TurboNet
 *
 * This enum defines all supported transport protocols. Each protocol is
 * implemented with the same API, eliminating transport-specific code duplication.
 */
typedef enum {
  TURBO_TCP = 0,    /**< Transmission Control Protocol - reliable, connection-oriented */
  TURBO_TLS,        /**< Transport Layer Security - secure TCP with encryption */
  TURBO_KCP,        /**< KCP Protocol - reliable UDP with congestion control */
  TURBO_UDP,        /**< User Datagram Protocol - unreliable, connectionless */
  TURBO_PIPE,       /**< Named pipes (Unix domain sockets) - local IPC */
  TURBO_QUIC,       /**< QUIC Protocol - secure UDP with multiplexing */
  TURBO_TRANSPORT_MAX /**< Maximum transport type value for bounds checking */
} turbo_transport_t;

/**
 * @brief Connection state enumeration for all transport protocols
 *
 * This enum defines the unified lifecycle states that apply to all transport
 * protocols. Each protocol implementation maps its internal states to these
 * common states for consistency.
 */
typedef enum {
  TURBO_CLOSED = 0,    /**< Connection is closed and handle is not usable */
  TURBO_CONNECTING,    /**< Connection attempt is in progress */
  TURBO_CONNECTED,     /**< Connection is established and ready for I/O */
  TURBO_CLOSING,       /**< Connection is being closed gracefully */
  TURBO_ERROR          /**< Connection is in an error state */
} turbo_state_t;

/**
 * @brief Buffer structure for data operations
 *
 * This simple buffer structure is used consistently across all transport
 * protocols for both reading and writing data.
 */
struct turbo_buf_s {
  char* base;    /**< Pointer to buffer data */
  size_t len;    /**< Length of data in buffer */
};

/**
 * @brief Enhanced error information structure
 *
 * This structure provides comprehensive error information beyond simple error
 * codes, including human-readable messages, context, technical details, and
 * timestamps to aid in debugging and monitoring.
 *
 * All error information is stored as human-readable strings to ensure
 * compatibility across platforms and ease of logging/serialization.
 */
struct turbo_error_s {
    int code;                       /**< Standard libuv/turbo error code */
    long timestamp;
    char message[256];              /**< Human-readable error message */
    char context[128];              /**< Context: URL, address, operation that failed */
    char details[128];              /**< Technical details: SSL errors, DNS failures, system errors */
};

// Unified callbacks - same signature across all transports
typedef void (*turbo_connect_cb)(turbo_handle_t* handle, int status);
typedef void (*turbo_read_cb)(turbo_handle_t* handle, ssize_t nread, const turbo_buf_t* buf);
typedef void (*turbo_write_cb)(turbo_req_t* req, int status);
typedef void (*turbo_close_cb)(turbo_handle_t* handle);
typedef void (*turbo_alloc_cb)(turbo_handle_t* handle, size_t suggested_size, turbo_buf_t* buf);
typedef void (*turbo_error_cb)(turbo_handle_t* handle, const turbo_error_t* error);

/**
 * @brief Main handle structure for all transport connections
 *
 * This is the core structure that represents a network connection. It works
 * uniformly across all transport protocols (TCP, TLS, UDP, KCP, etc.).
 *
 * Users should only interact with the public fields. Private fields are
 * managed internally and should not be modified.
 */
struct turbo_handle_s {
  // Public fields - users can read these
  turbo_transport_t transport;    /**< Transport protocol type */
  turbo_state_t state;            /**< Current connection state */
  void* data;                     /**< User-defined data pointer */

  // Connection info
  char remote_ip[46];             /**< Remote IP address (supports IPv6) */
  int remote_port;                /**< Remote port number */
  char local_ip[46];              /**< Local IP address (supports IPv6) */
  int local_port;                 /**< Local port number */

  // Statistics
  uint64_t bytes_read;            /**< Total bytes read */
  uint64_t bytes_written;         /**< Total bytes written */
  uint64_t connect_time;          /**< Connection timestamp (microseconds) */

  // Callbacks
  turbo_connect_cb connect_cb;    /**< Connection callback */
  turbo_read_cb read_cb;          /**< Read data callback */
  turbo_alloc_cb alloc_cb;        /**< Buffer allocation callback */
  turbo_close_cb close_cb;        /**< Connection close callback */
  turbo_error_cb error_cb;        /**< Error callback for non-fatal errors */

  // Private fields - DON'T TOUCH - these are opaque implementation details
  void* internal;                 /** Internal: transport-specific implementation */
  void* loop;                     /**< Event loop (uv_loop_t* but opaque to user) */
  turbo_mutex_t mutex;            /** Internal: thread safety */
  turbo_error_t last_error;       /** Internal: Last error that occurred */
};

// Write request structure
struct turbo_req_s {
  // Public
  void* data;  // user data
  turbo_write_cb write_cb;

  // Private - don't touch
  turbo_handle_t* handle;
  void* internal;  // transport-specific request data
};

// =============================================================================
// =============================================================================
// PRIMARY UNIFIED API - URLs are the future!
// =============================================================================

/**
 * @brief Connect to a remote endpoint using URL format
 *
 * This function eliminates transport-specific code by auto-detecting the transport
 * protocol from the URL scheme and initializing the handle automatically if needed.
 *
 * @param handle Pointer to turbo_handle_t, will be initialized if transport is TURBO_TCP
 * @param url URL string specifying transport and endpoint (e.g., "tcp://127.0.0.1:8080")
 * @param cb Callback function called when connection is established or fails
 * @return 0 on success, negative error code on failure
 *
 * @note Supported URL schemes: tcp, tls, udp, pipe, kcp, quic
 * @note Transport type is auto-detected from URL scheme
 * @note Handle is automatically initialized with the global event loop
 *
 * Example usage:
 * @code
 * turbo_global_init();
 * turbo_handle_t handle;
 * turbo_connect_url(&handle, "tls://example.com:443", on_connect);
 * turbo_run();
 * @endcode
 */
TURBONET_API int turbo_connect_url(turbo_handle_t* handle, const char* url, turbo_connect_cb cb);

/**
 * @brief Bind a handle to listen for incoming connections using URL format
 *
 * This function binds a handle to a local address and prepares it to accept
 * incoming connections. It auto-detects the transport protocol from the URL scheme.
 *
 * @param handle Pointer to turbo_handle_t to bind
 * @param url URL string specifying transport and local endpoint
 * @return 0 on success, negative error code on failure
 *
 * @note Supported URL schemes: tcp, tls, udp, pipe, kcp, quic
 * @note For TCP/TLS, binds to the specified IP and port
 * @note For pipes, binds to the named pipe/socket file
 * @note Handle is automatically initialized with the global event loop
 * @note Call turbo_listen() after binding for server mode
 *
 * Example usage:
 * @code
 * turbo_global_init();
 * turbo_handle_t handle;
 * turbo_bind_url(&handle, "tcp://0.0.0.0:8080");
 * turbo_listen(&handle, 10, on_connection);
 * turbo_run();
 * @endcode
 */
TURBONET_API int turbo_bind_url(turbo_handle_t* handle, const char* url);

/**
 * @brief Start listening for incoming connections
 *
 * This function configures the handle to listen for and accept incoming connections.
 * The same API works across all transport protocols.
 *
 * @param handle Pointer to turbo_handle_t that was previously bound
 * @param backlog Maximum number of pending connections in the queue
 * @param connection_cb Callback called when a new connection is received
 * @return 0 on success, negative error code on failure
 *
 * @note Handle must be bound to a local address before calling listen
 * @note The connection callback receives new client handles for each connection
 * @note Call turbo_accept() within the connection callback to accept the connection
 *
 * Example usage:
 * @code
 * void on_connection(turbo_handle_t* server, int status) {
 *     if (status == 0) {
 *         turbo_handle_t client;
 *         turbo_accept(server, &client);
 *         // Handle new client connection
 *     }
 * }
 *
 * turbo_bind_url(&server, "tcp://0.0.0.0:8080");
 * turbo_listen(&server, 128, on_connection);
 * @endcode
 */
TURBONET_API int turbo_listen(turbo_handle_t* handle, int backlog, turbo_connect_cb connection_cb);

// =============================================================================
// CORE I/O API - same for all approaches
// =============================================================================

/**
 * @brief Accept an incoming connection on a server handle
 *
 * This function accepts a pending incoming connection and initializes a new
 * client handle to handle the connection. The client handle is automatically
 * configured with the same transport type as the server.
 *
 * @param server Pointer to server turbo_handle_t that received new connection
 * @param client Pointer to client turbo_handle_t to initialize (must be zero-initialized)
 * @return 0 on success, negative error code on failure
 *
 * @note This function should be called from within the connection callback
 * @note The client handle is automatically initialized with the server's event loop
 * @note The client handle inherits the server's transport type automatically
 * @note After acceptance, the client handle is ready for I/O operations
 *
 * Example usage:
 * @code
 * void on_connection(turbo_handle_t* server, int status) {
 *     if (status == 0) {
 *         turbo_handle_t* client = malloc(sizeof(turbo_handle_t));
 *         memset(client, 0, sizeof(*client));
 *         client->data = my_user_data;
 *
 *         if (turbo_accept(server, client) == 0) {
 *             // Start reading or writing with client
 *             turbo_read_start(client, alloc_cb, read_cb);
 *         }
 *     }
 * }
 * @endcode
 */
TURBONET_API int turbo_accept(turbo_handle_t* server, turbo_handle_t* client);

/**
 * @brief Start reading data from a connection
 *
 * This function begins asynchronous reading of data from the connection.
 * Data will be delivered to the read callback as it becomes available.
 *
 * @param handle Pointer to turbo_handle_t to start reading from
 * @param alloc_cb Callback to allocate buffers for incoming data
 * @param read_cb Callback to handle received data
 * @return 0 on success, negative error code on failure
 *
 * @note The alloc callback is called to provide buffers for incoming data
 * @note The read callback receives data with nread indicating bytes received
 * @note If nread is 0, it indicates the connection was closed by the peer
 * @note If nread is negative, it indicates an error condition
 * @note Call turbo_read_stop() to stop reading
 *
 * Example usage:
 * @code
 * turbo_buf_t turbo_alloc_cb(turbo_handle_t* handle, size_t suggested_size, turbo_buf_t* buf) {
 *     buf->base = malloc(suggested_size);
 *     buf->len = suggested_size;
 *     return *buf;
 * }
 *
 * void turbo_read_cb(turbo_handle_t* handle, ssize_t nread, const turbo_buf_t* buf) {
 *     if (nread > 0) {
 *         // Process received data
 *         process_data(buf->base, nread);
 *         free(buf->base);
 *     } else if (nread == 0) {
 *         // Connection closed by peer
 *         turbo_close(handle, close_cb);
 *     } else {
 *         // Error occurred
 *         printf("Read error: %s\n", turbo_strerror(nread));
 *     }
 * }
 *
 * turbo_read_start(handle, turbo_alloc_cb, turbo_read_cb);
 * @endcode
 */
TURBONET_API int turbo_read_start(turbo_handle_t* handle, turbo_alloc_cb alloc_cb, turbo_read_cb read_cb);

/**
 * @brief Stop reading data from a connection
 *
 * This function stops asynchronous reading that was previously started
 * with turbo_read_start().
 *
 * @param handle Pointer to turbo_handle_t to stop reading from
 * @return 0 on success, negative error code on failure
 *
 * @note No more read callbacks will be called after calling this function
 * @note The alloc callback will also stop being called
 * @note This does not close the connection, just stops reading
 *
 * Example usage:
 * @code
 * // Temporarily stop reading
 * turbo_read_stop(handle);
 *
 * // Later resume reading
 * turbo_read_start(handle, alloc_cb, read_cb);
 * @endcode
 */
TURBONET_API int turbo_read_stop(turbo_handle_t* handle);

/**
 * @brief Write data asynchronously to a connection
 *
 * This function sends data to the connected peer asynchronously. The write
 * operation completes when the write callback is called with the result.
 *
 * @param req Pointer to turbo_req_t to track the write operation
 * @param handle Pointer to turbo_handle_t to write to
 * @param bufs Array of turbo_buf_t structures containing data to write
 * @param nbufs Number of buffers in the bufs array
 * @param cb Callback function called when write completes
 * @return 0 on success, negative error code on failure
 *
 * @note The buffers are not copied, so they must remain valid until the callback
 * @note The request structure is used to track the operation
 * @note Multiple buffers can be written efficiently in a single operation
 * @note The callback indicates success or failure of the write
 *
 * Example usage:
 * @code
 * void write_callback(turbo_req_t* req, int status) {
 *     if (status == 0) {
 *         printf("Data written successfully\n");
 *     } else {
 *         printf("Write failed: %s\n", turbo_strerror(status));
 *     }
 *
 *     // Clean up buffers if needed
 *     for (unsigned int i = 0; i < req->nbufs; i++) {
 *         free(req->bufs[i].base);
 *     }
 *
 *     free(req);
 * }
 *
 * void send_data(turbo_handle_t* handle, const char* data, size_t len) {
 *     // Prepare buffers (scatter-gather I/O)
 *     turbo_buf_t buf1 = { .base = (char*)data, .len = len / 2 };
 *     turbo_buf_t buf2 = { .base = (char*)data + len / 2, .len = len - len / 2 };
 *     turbo_buf_t bufs[] = { buf1, buf2 };
 *
 *     // Create request
 *     turbo_req_t* req = calloc(1, sizeof(turbo_req_t));
 *     req->write_cb = write_callback;
 *
 *     turbo_write(req, handle, bufs, 2, write_callback);
 * }
 * @endcode
 */
TURBONET_API int turbo_write(turbo_req_t* req,
                turbo_handle_t* handle,
                const turbo_buf_t bufs[],
                unsigned int nbufs,
                turbo_write_cb cb);

/**
 * @brief Close a connection gracefully
 *
 * This function initiates a graceful shutdown of the connection. The close
 * callback will be called when the connection is fully closed.
 *
 * @param handle Pointer to turbo_handle_t to close
 * @param close_cb Callback function called when connection is fully closed
 * @return 0 on success, negative error code on failure
 *
 * @note This function is asynchronous - the close callback indicates completion
 * @note After calling close, no more I/O operations should be performed
 * @note The handle structure should not be used after the close callback
 * @note Resources are cleaned up automatically when the close callback is called
 *
 * Example usage:
 * @code
 * void close_callback(turbo_handle_t* handle) {
 *     printf("Connection closed\n");
 *     free(handle);  // Safe to free handle now
 * }
 *
 * // Close the connection
 * turbo_close(handle, close_callback);
 * @endcode
 */
TURBONET_API int turbo_close(turbo_handle_t* handle, turbo_close_cb close_cb);

/**
 * @brief Validate a URL and determine its transport protocol
 *
 * This function parses a URL and determines what transport protocol it specifies.
 * It's used internally by the URL-based connection functions.
 *
 * @param url URL string to validate and parse
 * @param transport Pointer to turbo_transport_t to store detected transport type
 * @return 0 on success, negative error code on failure
 *
 * @note Supported URL schemes: tcp, tls, udp, pipe, kcp, quic
 * @note Returns specific error codes for invalid URLs
 */
TURBONET_API int turbo_validate_url(const char* url, turbo_transport_t* transport);

/**
 * @brief Get the URL scheme string for a transport protocol
 *
 * This function returns the standard URL scheme string for a given transport
 * protocol, useful for constructing URLs or debugging.
 *
 * @param transport Transport protocol to get scheme for
 * @return URL scheme string (e.g., "tcp", "tls", "udp"), or NULL if invalid
 */
TURBONET_API const char* turbo_get_scheme_for_transport(turbo_transport_t transport);

/**
 * @brief Get the default port number for a transport protocol
 *
 * This function returns the standard default port number for a given transport
 * protocol as defined by IANA standards.
 *
 * @param transport Transport protocol to get default port for
 * @return Default port number, or 0 if transport doesn't have a standard port
 *
 * @note TCP/443 defaults to 80 for unencrypted, 443 for TLS
 * @note UDP/53 defaults to 53 (DNS)
 * @note PIPE returns 0 (no default port concept)
 */
TURBONET_API int turbo_get_default_port(turbo_transport_t transport);

// DNS Resolution Callback
typedef void (*turbo_resolve_cb)(const char* hostname, const char* ip, int status, void* user_data);

/**
 * @brief Simple hostname to IP resolution
 *
 * This function performs DNS resolution for a hostname and returns the result
 * via callback. Fast path for IP addresses, async DNS for hostnames.
 *
 * @param loop Event loop to use for async DNS resolution
 * @param hostname Hostname or IP address to resolve
 * @param callback Callback function called with result
 * @param user_data User data passed to callback
 * @return 0 on success, negative error code on failure
 *
 * @note For direct IP addresses, callback is called immediately
 * @note For hostnames, DNS resolution is performed asynchronously
 * @note Supports both IPv4 and IPv6 addresses
 *
 * Example usage:
 * @code
 * void dns_cb(const char* hostname, const char* ip, int status, void* data) {
 *     if (status == 0) {
 *         printf("Resolved %s -> %s\n", hostname, ip);
 *         // Now use ip for connection
 *     } else {
 *         printf("DNS failed: %s\n", uv_strerror(status));
 *     }
 * }
 * 
 * turbo_resolve_hostname(loop, "google.com", dns_cb, my_data);
 * @endcode
 */
TURBONET_API int turbo_resolve_hostname(uv_loop_t* loop, const char* hostname, 
                                       turbo_resolve_cb callback, void* user_data);

/**
 * @brief Parse address string into sockaddr structure
 *
 * This function attempts to parse an address string as IPv4 or IPv6
 * and fill the appropriate sockaddr structure.
 *
 * @param address Address string to parse (IPv4 or IPv6)
 * @param port Port number to include in sockaddr
 * @param addr Output sockaddr_storage structure
 * @return 0 on success, UV_EAI_NONAME if not a valid IP address
 */
TURBONET_API int turbo_parse_address(const char* address, int port, struct sockaddr_storage* addr);

// =============================================================================
// Configuration API - transport-specific options
// =============================================================================

// Generic option setting - key-value pairs
TURBONET_API int turbo_set_option(turbo_handle_t* handle, const char* key, const void* value, size_t len);
TURBONET_API int turbo_get_option(turbo_handle_t* handle, const char* key, void* value, size_t* len);

// Common options (shortcuts for frequent use)
TURBONET_API int turbo_set_timeout(turbo_handle_t* handle, uint32_t timeout_ms);
TURBONET_API int turbo_set_keepalive(turbo_handle_t* handle, int enable, uint32_t delay);
TURBONET_API int turbo_set_nodelay(turbo_handle_t* handle, int enable);

// TLS-specific options
TURBONET_API int turbo_tls_set_cert(turbo_handle_t* handle, const char* cert_file, const char* key_file);
TURBONET_API int turbo_tls_set_ca(turbo_handle_t* handle, const char* ca_file);
TURBONET_API int turbo_tls_set_verify(turbo_handle_t* handle, int verify_peer);
TURBONET_API int turbo_tls_set_cipher_list(turbo_handle_t* handle, const char* cipher_list);  // Set allowed ciphers
TURBONET_API int turbo_tls_set_secure_defaults(turbo_handle_t* handle);  // Set modern secure configuration

// KCP-specific options
TURBONET_API int turbo_kcp_set_mode(turbo_handle_t* handle, int mode);  // 0=default, 1=fast
TURBONET_API int turbo_kcp_set_wndsize(turbo_handle_t* handle, int snd_wnd, int rcv_wnd);

// =============================================================================
// Enhanced Error Handling System
// =============================================================================

/**
 * @brief Get the last detailed error information for a handle
 *
 * This function retrieves the most recent error that occurred on the handle,
 * including detailed context and technical information.
 *
 * @param handle Pointer to turbo_handle_t to get error information from
 * @param error Pointer to turbo_error_t structure to fill with error details
 * @return 0 on success (even if there was an error), negative error code on failure
 *
 * @note This function returns 0 even when there was an error to be retrieved
 * @note Check the error->code field to see if there was actually an error
 * @note If error->code is 0, no error has occurred
 */
TURBONET_API int turbo_get_last_error(turbo_handle_t* handle, turbo_error_t* error);

/**
 * @brief Clear the error state of a handle
 *
 * This function resets the error state of the handle, allowing new operations
 * to proceed without being affected by previous error conditions.
 *
 * @param handle Pointer to turbo_handle_t to clear error state for
 *
 * @note Error state is automatically cleared when new operations begin
 * @note This function is mostly useful for manually resetting error state
 * @note Does not affect the currently executing operation
 */
TURBONET_API void turbo_clear_error(turbo_handle_t* handle);

/**
 * @brief Set a detailed error with context information
 *
 * This function creates a comprehensive error record with human-readable
 * message, context, and technical details that aid in debugging.
 *
 * @param handle Pointer to turbo_handle_t to associate error with
 * @param code Standard libuv/turbo error code
 * @param context Human-readable description of when/where error occurred
 * @param details Technical details like system error codes, SSL errors, etc.
 *
 * @note Error callbacks will be invoked if one is registered
 * @note Error information is stored in handle->last_error
 * @note Context and details provide debugging information for developers
 */
TURBONET_API void turbo_set_error(turbo_handle_t* handle, int code,
                    const char* context, const char* details);

/**
 * @brief Set a callback function to handle non-fatal errors
 *
 * This function registers a callback that will be called whenever non-fatal
 * errors occur on the handle, allowing for custom error handling and logging.
 *
 * @param handle Pointer to turbo_handle_t to set error callback for
 * @param error_cb Callback function to handle errors, or NULL to disable
 * @return 0 on success, negative error code on failure
 *
 * @note Callback is called for non-fatal errors only
 * @note Fatal errors still cause operation failure but don't call this callback
 * @note Error callback receives detailed error information structure
 */
TURBONET_API int turbo_set_error_callback(turbo_handle_t* handle, turbo_error_cb error_cb);

// =============================================================================
// Utility functions
// =============================================================================

TURBONET_API const char* turbo_strerror(int err);

// =============================================================================
// Event Loop API - hides libuv implementation
// =============================================================================

/**
 * @brief Initialize TurboNet global state and set up handle
 *
 * This function initializes the global event loop and sets up the handle
 * to use the global loop. This is the preferred way to initialize handles.
 *
 * @param handle Handle to initialize with global loop
 * @return 0 on success, negative error code on failure
 *
 * @note This function is thread-safe and can be called multiple times
 * @note Only the first call performs actual global initialization
 * @note Handle will be zeroed and loop field set to global loop
 */
TURBONET_API int turbo_global_init(turbo_handle_t* handle);

/**
 * @brief Set DNS servers for hostname resolution
 *
 * This function configures custom DNS servers for hostname resolution.
 * Must be called before any DNS resolution attempts.
 *
 * @param servers Array of DNS server IP addresses
 * @param count Number of DNS servers in the array
 * @return 0 on success, negative error code on failure
 *
 * @note Call this before turbo_global_init() or any connection attempts
 * @note Servers should be IPv4 or IPv6 addresses, not hostnames
 * @note If not called, system default DNS servers are used
 */
TURBONET_API int turbo_set_dns_servers(const char* servers[], int count);

/**
 * @brief Get currently configured DNS servers
 *
 * @param servers Buffer to store DNS server addresses
 * @param max_servers Maximum number of servers to return
 * @param count Pointer to store actual number of servers returned
 * @return 0 on success, negative error code on failure
 */
TURBONET_API int turbo_get_dns_servers(char servers[][46], int max_servers, int* count);

/**
 * @brief Run the TurboNet event loop
 *
 * This function runs the event loop, processing all pending events
 * and I/O operations until there are no more active handles.
 *
 * @return 0 on success, negative error code on failure
 *
 * @note This function blocks until all handles are closed
 * @note Equivalent to uv_run(UV_RUN_DEFAULT) but hides libuv details
 * @note Use turbo_run_once() for non-blocking operation
 */
TURBONET_API int turbo_run(void);

/**
 * @brief Run the TurboNet event loop once (non-blocking)
 *
 * This function processes one iteration of the event loop and returns
 * immediately. Use this when you need to integrate with your own
 * main loop or do other work between I/O operations.
 *
 * @return 1 if there are pending events, 0 if no more events, negative on error
 *
 * @note This function does not block
 * @note Call this repeatedly in your own loop
 * @note Equivalent to uv_run(UV_RUN_NOWAIT) but hides libuv details
 */
TURBONET_API int turbo_run_once(void);

/**
 * @brief Run the TurboNet event loop until no pending events
 *
 * This function processes all pending events and returns when the
 * event loop becomes idle (no pending I/O or timers).
 *
 * @return 0 on success, negative error code on failure
 *
 * @note This function may block briefly but returns when idle
 * @note Useful for processing all pending events without waiting
 * @note Equivalent to uv_run(UV_RUN_ONCE) but hides libuv details
 */
TURBONET_API int turbo_run_nowait(void);

/**
 * @brief Stop the TurboNet event loop
 *
 * This function requests the event loop to stop processing events.
 * The loop will stop after the current iteration completes.
 *
 * @note This is asynchronous - the loop may not stop immediately
 * @note Use this to gracefully shutdown the event loop
 */
TURBONET_API void turbo_stop(void);

/**
 * @brief Cleanup TurboNet global state
 *
 * This function cleans up global state and the event loop.
 * Should be called when shutting down the application.
 *
 * @note This function is thread-safe
 * @note Can be called multiple times safely
 */
TURBONET_API void turbo_global_cleanup(void);

// Convert error struct to formatted string
TURBONET_API int turbo_error_to_string(const turbo_error_t* error, char* buffer, size_t buffer_size);
// Get transport name as string
TURBONET_API const char* turbo_transport_name(turbo_transport_t transport);

// Get state name as string
TURBONET_API const char* turbo_state_name(turbo_state_t state);

// Buffer helpers
TURBONET_API turbo_buf_t turbo_buf_init(char* base, size_t len);
TURBONET_API void turbo_buf_free(turbo_buf_t* buf);
 
#ifdef __cplusplus
}
#endif

#endif  // TURBONET_H
