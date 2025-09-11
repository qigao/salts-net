/**
 * Internal transport adapter interface
 * "Separate policy from mechanism" - Linus
 */
#ifndef TURBONET_INTERNAL_H
#define TURBONET_INTERNAL_H

#include "turbonet.h"
#include <uv.h>
// Transport adapter interface - ALL transports implement this
typedef struct turbo_transport_vtable_s {
    // Core operations - MUST be implemented by all transports
    int (*init)(turbo_handle_t* handle);
    int (*connect)(turbo_handle_t* handle, const char* address, int port);
    int (*bind)(turbo_handle_t* handle, const char* address, int port);
    int (*listen)(turbo_handle_t* handle, int backlog);
    int (*accept)(turbo_handle_t* server, turbo_handle_t* client);
    int (*read_start)(turbo_handle_t* handle);
    int (*read_stop)(turbo_handle_t* handle);
    int (*write)(turbo_req_t* req, const turbo_buf_t bufs[], unsigned int nbufs);
    int (*close)(turbo_handle_t* handle);
    int (*cleanup)(turbo_handle_t* handle);
    
    // Optional operations - can be NULL if not supported
    int (*set_option)(turbo_handle_t* handle, const char* key, const void* value, size_t len);
    int (*get_option)(turbo_handle_t* handle, const char* key, void* value, size_t* len);
} turbo_transport_vtable_t;

// Internal handle data - what's hidden inside turbo_handle_t.internal
typedef struct turbo_internal_s {
    const turbo_transport_vtable_t* vtable;
    void* transport_data;  // transport-specific data
    
    // Common state across all transports
    bool is_server;
    bool reading;
    bool closing;
    
    // Reference counting for thread safety
    int ref_count;
} turbo_internal_t;

// Transport registration - called during library initialization
void turbo_register_transport(turbo_transport_t type, const turbo_transport_vtable_t* vtable);

// Internal utilities
turbo_internal_t* turbo_get_internal(turbo_handle_t* handle);
int turbo_call_vtable(turbo_handle_t* handle, const char* method, ...);

// =============================================================================
// INTERNAL CORE TRANSPORT API - used by URL API and transport implementations
// =============================================================================

// Initialize handle for specific transport (used internally by URL API)
int turbo_init(turbo_handle_t* handle, uv_loop_t* loop, turbo_transport_t transport);

// Connect to remote endpoint (used internally by URL API)
int turbo_connect(turbo_handle_t* handle, const char* address, int port, turbo_connect_cb cb);

// Bind for incoming connections (used internally by URL API)
int turbo_bind(turbo_handle_t* handle, const char* address, int port);

// Initialize handle for URL-based connection (used internally)
// Auto-detects transport from URL scheme
int turbo_init_url(turbo_handle_t* handle, uv_loop_t* loop, const char* url);

// Error codes - extend libuv error codes
#define TURBO_EUNSUPPORTED     (-4000)  // Transport doesn't support this operation
#define TURBO_EINVAL_TRANSPORT (-4001)  // Invalid transport type
#define TURBO_EALREADY_INIT    (-4002)  // Already initialized
#define TURBO_ENOTINIT         (-4003)  // Not initialized
#define TURBO_ETLS_HANDSHAKE   (-4004)  // TLS handshake failed
#define TURBO_ETLS_CERT        (-4005)  // TLS certificate error  
#define TURBO_EKCP_PROTOCOL    (-4006)  // KCP protocol error
#define TURBO_EQUIC_PROTOCOL   (-4007)  // QUIC protocol error

// =============================================================================
// Internal Error Handling Helpers - for transport implementations
// =============================================================================

// Set error with system errno details
void turbo_set_error_with_errno(turbo_handle_t* handle, int code, 
                                const char* context, int errno_val);

// Set TLS-specific error with OpenSSL error details  
void turbo_set_tls_error(turbo_handle_t* handle, int code,
                        const char* context, unsigned long ssl_error);

// Set DNS resolution error with hostname context
void turbo_set_dns_error(turbo_handle_t* handle, int code,
                        const char* hostname, const char* error_details);

// =============================================================================
// Internal Address Resolution Helpers - for transport implementations
// =============================================================================

// Unified address resolution - handles IPv4/IPv6/DNS automatically
// Returns 0 if resolved synchronously, UV_EAGAIN if async DNS started
// For sync resolution, result is stored in resolved_addr
// For async resolution, callback will be called when complete
typedef void (*turbo_address_resolved_cb)(turbo_handle_t* handle, int status, 
                                          const struct sockaddr* addr, int addrlen);

int turbo_resolve_address(turbo_handle_t* handle, const char* address, int port,
                         struct sockaddr_storage* resolved_addr,
                         turbo_address_resolved_cb callback);

// Direct address parsing (fast path) - no DNS, just IPv4/IPv6
// Returns 0 on success, UV_EINVAL if not a valid IP address
int turbo_parse_address(const char* address, int port, struct sockaddr_storage* addr);

#endif // TURBONET_INTERNAL_H