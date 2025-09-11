/**
 * Unified Address Parsing for TurboNet
 * "Eliminate special cases, don't add them" - Linus
 * 
 * Uses the RFC-compliant URI parser to provide unified addressing
 * across all transport protocols. No more special cases!
 */
#include "turbonet.h"
#include "turbonet_internal.h"
#include "uri.h"  // Our RFC-compliant parser
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// Address info extracted from URL - stack allocated, no malloc!
typedef struct {
    turbo_transport_t transport;
    char host[256];
    int port;
    char path[1024];  // For PIPE sockets
    host_type_t host_type;
    bool valid;
} turbo_address_t;

// Transport scheme mapping - simple lookup table
static const struct {
    const char* scheme;
    turbo_transport_t transport;
    int default_port;
} g_transport_schemes[] = {
    {"tcp",   TURBO_TCP,   80},
    {"tls",   TURBO_TLS,   443},
    {"ssl",   TURBO_TLS,   443},   // alias for tls
    {"https", TURBO_TLS,   443},   // alias for tls  
    {"udp",   TURBO_UDP,   53},
    {"pipe",  TURBO_PIPE,  0},
    {"kcp",   TURBO_KCP,   8888},
    {"quic",  TURBO_QUIC,  443},
    {"http3", TURBO_QUIC,  443},   // alias for quic
    {NULL,    TURBO_TCP,   80}     // sentinel
};

// Helper to convert string to lower case - avoids non-standard stricmp
static void to_lower(char* str) {
    if (!str) return;
    for (; *str; ++str) {
        *str = tolower((unsigned char)*str);
    }
}

// Parse URL into transport and connection details
static int parse_transport_url(const char* url, turbo_address_t* addr) {
    if (!url || !addr) {
        return UV_EINVAL;
    }
    
    // Clear result
    memset(addr, 0, sizeof(*addr));
    
    // Use our RFC-compliant parser - no malloc needed!
    url_t parsed_url;
    if (!parse_url(url, &parsed_url) || !parsed_url.valid) {
        log_error("Invalid URL format: %s", url);
        return TURBO_EINVAL_TRANSPORT;
    }
    
    // RFCs state scheme is case-insensitive, so convert to lower
    to_lower(parsed_url.scheme);

    // Map scheme to transport type
    turbo_transport_t transport = TURBO_TCP;  // default
    int default_port = 80;
    bool found = false;
    
    for (int i = 0; g_transport_schemes[i].scheme; i++) {
        if (strcmp(parsed_url.scheme, g_transport_schemes[i].scheme) == 0) {
            transport = g_transport_schemes[i].transport;
            default_port = g_transport_schemes[i].default_port;
            found = true;
            break;
        }
    }
    
    if (!found) {
        log_error("Unsupported transport scheme: %s", parsed_url.scheme);
        return TURBO_EINVAL_TRANSPORT;
    }
    
    // Extract connection details based on transport type
    addr->transport = transport;
    addr->host_type = parsed_url.host_type;
    
    if (transport == TURBO_PIPE) {
        // For PIPE, construct platform-specific socket address
#ifdef _WIN32
        // Windows named pipe format: \\.\pipe\name
        if (strlen(parsed_url.path) > 1) {  // Skip leading '/'
            snprintf(addr->path, sizeof(addr->path) - 1, "\\\\.\\pipe\\%s", parsed_url.path + 1);
        } else if (strlen(parsed_url.host) > 0 && strcmp(parsed_url.host, ".") != 0) {
            snprintf(addr->path, sizeof(addr->path) - 1, "\\\\.\\pipe\\%s", parsed_url.host);  
        } else {
            snprintf(addr->path, sizeof(addr->path) - 1, "\\\\.\\pipe\\turbonet_default");
        }
#else
        // Unix domain socket format: use path directly
        if (strlen(parsed_url.path) > 0) {
            strncpy(addr->path, parsed_url.path, sizeof(addr->path) - 1);
        } else {
            strncpy(addr->path, parsed_url.host, sizeof(addr->path) - 1);
        }
#endif
        addr->port = 0;  // No port for PIPE
    } else {
        // For network transports, use host:port
        strncpy(addr->host, parsed_url.host, sizeof(addr->host) - 1);
        addr->port = parsed_url.port > 0 ? parsed_url.port : default_port;
        
        // Validate host type for network protocols
        if (addr->host_type == HOST_UNKNOWN) {
            log_error("Invalid host in URL: %s", url);
            return TURBO_EINVAL_TRANSPORT;
        }
    }
    
    addr->valid = true;
    log_debug("Parsed URL: %s -> %s transport, host=%s, port=%d", 
             url, turbo_transport_name(transport), addr->host, addr->port);
    
    return 0;
}

// =============================================================================
// DNS Resolution Adapters - bridge new DNS API to old connect flow
// =============================================================================

typedef struct {
    turbo_handle_t* handle;
    turbo_connect_cb connect_cb;
    int port;
} dns_connect_ctx_t;

typedef struct {
    turbo_handle_t* handle;
    int port;
} dns_bind_ctx_t;

// Callback when DNS resolves for connect
static void dns_connect_resolved(const char* hostname, const char* ip, int status, void* user_data) {
    dns_connect_ctx_t* ctx = (dns_connect_ctx_t*)user_data;
    
    if (status != 0) {
        log_error("DNS resolution failed for %s: %s", hostname, uv_strerror(status));
        ctx->connect_cb(ctx->handle, status);
        free(ctx);
        return;
    }
    
    // DNS resolved successfully, now do regular connect with IP
    log_debug("DNS resolved %s -> %s, connecting", hostname, ip);
    
    // Get transport vtable and call direct connect
    turbo_internal_t* internal = turbo_get_internal(ctx->handle);
    if (!internal || !internal->vtable || !internal->vtable->connect) {
        ctx->connect_cb(ctx->handle, TURBO_EUNSUPPORTED);
        free(ctx);
        return;
    }
    
    int err = internal->vtable->connect(ctx->handle, ip, ctx->port);
    if (err != 0) {
        ctx->connect_cb(ctx->handle, err);
    }
    // Success case: vtable->connect will call connect_cb when done
    
    free(ctx);
}

// Callback when DNS resolves for bind  
static void dns_bind_resolved(const char* hostname, const char* ip, int status, void* user_data) {
    dns_bind_ctx_t* ctx = (dns_bind_ctx_t*)user_data;
    
    if (status != 0) {
        log_error("DNS resolution failed for %s: %s", hostname, uv_strerror(status));
        free(ctx);
        return;
    }
    
    // DNS resolved successfully, now do regular bind with IP
    log_debug("DNS resolved %s -> %s, binding", hostname, ip);
    
    // Get transport vtable and call direct bind
    turbo_internal_t* internal = turbo_get_internal(ctx->handle);
    if (!internal || !internal->vtable || !internal->vtable->bind) {
        log_error("Bind not supported for this transport");
        free(ctx);
        return;
    }
    
    int err = internal->vtable->bind(ctx->handle, ip, ctx->port);
    if (err != 0) {
        log_error("Bind failed after DNS resolution: %s", uv_strerror(err));
    }
    
    free(ctx);
}

// Replacement for turbo_resolve_and_connect
static int turbo_resolve_and_connect_internal(turbo_handle_t* handle, const char* address, int port, turbo_connect_cb cb) {
    if (!handle || !address || !cb) {
        return UV_EINVAL;
    }
    
    // Fast path: try direct IP first
    struct sockaddr_storage addr;
    if (turbo_parse_address(address, port, &addr) == 0) {
        // Direct IP - use existing connect
        turbo_internal_t* internal = turbo_get_internal(handle);
        if (!internal || !internal->vtable || !internal->vtable->connect) {
            return TURBO_EUNSUPPORTED;
        }
        
        return internal->vtable->connect(handle, address, port);
    }
    
    // Slow path: DNS resolution needed
    dns_connect_ctx_t* ctx = malloc(sizeof(dns_connect_ctx_t));
    if (!ctx) {
        return UV_ENOMEM;
    }
    
    ctx->handle = handle;
    ctx->connect_cb = cb;
    ctx->port = port;
    
    return turbo_resolve_hostname(handle->loop, address, dns_connect_resolved, ctx);
}

// Replacement for turbo_resolve_and_bind
static int turbo_resolve_and_bind_internal(turbo_handle_t* handle, const char* address, int port) {
    if (!handle || !address) {
        return UV_EINVAL;
    }
    
    // Fast path: try direct IP first  
    struct sockaddr_storage addr;
    if (turbo_parse_address(address, port, &addr) == 0) {
        // Direct IP - use existing bind
        turbo_internal_t* internal = turbo_get_internal(handle);
        if (!internal || !internal->vtable || !internal->vtable->bind) {
            return TURBO_EUNSUPPORTED;
        }
        
        return internal->vtable->bind(handle, address, port);
    }
    
    // Slow path: DNS resolution needed
    dns_bind_ctx_t* ctx = malloc(sizeof(dns_bind_ctx_t));
    if (!ctx) {
        return UV_ENOMEM;
    }
    
    ctx->handle = handle;
    ctx->port = port;
    
    return turbo_resolve_hostname(handle->loop, address, dns_bind_resolved, ctx);
}

int turbo_init_url(turbo_handle_t* handle, uv_loop_t* loop, const char* url) {
    if (!handle || !loop || !url) {
        return UV_EINVAL;
    }
    
    // Extract transport from URL
    turbo_transport_t transport;
    int err = turbo_validate_url(url, &transport);
    if (err != 0) {
        log_error("Invalid URL for initialization: %s", url);
        return err;
    }
    
    // Initialize with detected transport
    return turbo_init(handle, loop, transport);
}

TURBONET_API int turbo_connect_url(turbo_handle_t* handle, const char* url, turbo_connect_cb cb) {
    if (!handle || !url || !cb) {
        return UV_EINVAL;
    }
    
    // Parse the URL to get transport and address details
    turbo_address_t addr;
    int err = parse_transport_url(url, &addr);
    if (err != 0) {
        return err;
    }
    
    // Auto-initialize handle if not initialized at all or has wrong transport
    // Users just need to provide a zero-initialized handle with loop set
    if (handle->internal == NULL || handle->transport != addr.transport) {
        if (handle->loop == NULL) {
            log_error("Handle must have loop set. Initialize: handle.loop = uv_default_loop();");
            return UV_EINVAL;
        }
        
        // Clean up old transport if any
        if (handle->internal != NULL) {
            turbo_close(handle, NULL);  
        }
        
        // Auto-initialize with correct transport
        err = turbo_init(handle, handle->loop, addr.transport);
        if (err != 0) {
            return err;
        }
        
        log_debug("Auto-initialized handle for %s transport from URL", turbo_transport_name(addr.transport));
    }
    
    // UNIFIED CALL - with DNS resolution support!
    const char* address = (addr.transport == TURBO_PIPE) ? addr.path : addr.host;
    
    if (addr.transport == TURBO_PIPE) {
        // PIPE sockets don't need DNS resolution
        return turbo_connect(handle, address, addr.port, cb);
    } else {
        // Network transports: use DNS-capable connect
        return turbo_resolve_and_connect_internal(handle, address, addr.port, cb);
    }
}

TURBONET_API int turbo_bind_url(turbo_handle_t* handle, const char* url) {
    if (!handle || !url) {
        return UV_EINVAL;
    }
    
    // Parse the URL to get transport and address details
    turbo_address_t addr;
    int err = parse_transport_url(url, &addr);
    if (err != 0) {
        return err;
    }
    
    // Auto-initialize handle if not initialized at all or has wrong transport
    // Users just need to provide a zero-initialized handle with loop set
    if (handle->internal == NULL || handle->transport != addr.transport) {
        if (handle->loop == NULL) {
            log_error("Handle must have loop set. Initialize: handle.loop = uv_default_loop();");
            return UV_EINVAL;
        }
        
        // Clean up old transport if any
        if (handle->internal != NULL) {
            turbo_close(handle, NULL);  
        }
        
        // Auto-initialize with correct transport
        err = turbo_init(handle, handle->loop, addr.transport);
        if (err != 0) {
            return err;
        }
        
        log_debug("Auto-initialized handle for %s transport from URL", turbo_transport_name(addr.transport));
    }
    
    // UNIFIED CALL - with DNS resolution support!
    const char* address = (addr.transport == TURBO_PIPE) ? addr.path : addr.host;
    
    if (addr.transport == TURBO_PIPE) {
        // PIPE sockets don't need DNS resolution
        return turbo_bind(handle, address, addr.port);
    } else {
        // Network transports: use DNS-capable bind
        return turbo_resolve_and_bind_internal(handle, address, addr.port);
    }
}

// =============================================================================
// Helper functions for address validation and conversion
// =============================================================================

TURBONET_API int turbo_validate_url(const char* url, turbo_transport_t* transport) {
    if (!url || !transport) {
        return UV_EINVAL;
    }
    
    turbo_address_t addr;
    int err = parse_transport_url(url, &addr);
    if (err == 0 && transport) {
        *transport = addr.transport;
    }
    return err;
}

TURBONET_API const char* turbo_get_scheme_for_transport(turbo_transport_t transport) {
    for (int i = 0; g_transport_schemes[i].scheme; i++) {
        if (g_transport_schemes[i].transport == transport) {
            return g_transport_schemes[i].scheme;
        }
    }
    return "tcp";  // fallback
}

TURBONET_API int turbo_get_default_port(turbo_transport_t transport) {
    for (int i = 0; g_transport_schemes[i].scheme; i++) {
        if (g_transport_schemes[i].transport == transport) {
            return g_transport_schemes[i].default_port;
        }
    }
    return 80;  // fallback
}
