/**
 * @file turbo_error.c
 * @brief Enhanced Error Handling System Implementation
 * @author "Good error messages save debugging time" - Linus Torvalds
 *
 * This file implements TurboNet's comprehensive error handling system that provides
 * detailed error information across all transport protocols. The system captures
 * context, timestamps, and technical details to aid in debugging and monitoring.
 *
 * Key features:
 * - Unified error codes across all transports
 * - Rich error context and technical details
 * - Timestamped error logging
 * - Human-readable error messages
 * - Integration with transport-specific error details (SSL, DNS, etc.)
 */
#include "turbonet_internal.h"
#include "turbonet.h"
#include "log.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#define HAVE_OPENSSL 1  // Assume OpenSSL on Windows
#else
#include <sys/time.h>
#endif

#ifdef HAVE_OPENSSL
#include <openssl/err.h>
#endif

// Error code to human-readable message mapping
typedef struct {
    int code;
    const char* message;
} turbo_error_mapping_t;

// Standard error messages - covers both libuv and turbo-specific errors
static const turbo_error_mapping_t g_error_messages[] = {
    // libuv errors - only include ones that exist across platforms
    {UV_EACCES,      "Permission denied"},
    {UV_EADDRINUSE,  "Address already in use"},
    {UV_EADDRNOTAVAIL, "Address not available"},
    {UV_EAFNOSUPPORT, "Address family not supported"},
    {UV_EAGAIN,      "Resource temporarily unavailable"},
    {UV_EAI_ADDRFAMILY, "Address family not supported"},
    {UV_EAI_AGAIN,   "Temporary DNS failure"},
    {UV_EAI_BADFLAGS, "Invalid DNS query flags"},
    {UV_EAI_BADHINTS, "Invalid hints"},
    {UV_EAI_CANCELED, "DNS request canceled"},
    {UV_EAI_FAIL,    "Permanent DNS failure"},
    {UV_EAI_FAMILY,  "Address family not supported"},
    {UV_EAI_MEMORY,  "DNS query out of memory"},
    {UV_EAI_NODATA,  "No address associated with hostname"},
    {UV_EAI_NONAME,  "Name or service not known"},
    {UV_EAI_OVERFLOW, "Argument buffer too small"},
    {UV_EAI_PROTOCOL, "Protocol not supported"},
    {UV_EAI_SERVICE, "Service not supported"},
    {UV_EAI_SOCKTYPE, "Socket type not supported"},
    {UV_EALREADY,    "Operation already in progress"},
    {UV_EBADF,       "Bad file descriptor"},
    {UV_EBUSY,       "Device or resource busy"},
    {UV_ECANCELED,   "Operation canceled"},
    {UV_ECHARSET,    "Invalid Unicode character"},
    {UV_ECONNABORTED, "Connection aborted"},
    {UV_ECONNREFUSED, "Connection refused"},
    {UV_ECONNRESET,  "Connection reset by peer"},
    {UV_EEXIST,      "File already exists"},
    {UV_EFAULT,      "Bad address"},
    {UV_EFBIG,       "File too large"},
    {UV_EHOSTUNREACH, "No route to host"},
    {UV_EINTR,       "Interrupted system call"},
    {UV_EINVAL,      "Invalid argument"},
    {UV_EIO,         "Input/output error"},
    {UV_EISCONN,     "Socket is already connected"},
    {UV_EISDIR,      "Illegal operation on directory"},
    {UV_ELOOP,       "Too many symbolic links"},
    {UV_EMFILE,      "Too many open files"},
    {UV_EMSGSIZE,    "Message too long"},
    {UV_ENAMETOOLONG, "Name too long"},
    {UV_ENETDOWN,    "Network is down"},
    {UV_ENETUNREACH, "Network unreachable"},
    {UV_ENFILE,      "File table overflow"},
    {UV_ENOBUFS,     "No buffer space available"},
    {UV_ENODEV,      "No such device"},
    {UV_ENOENT,      "No such file or directory"},
    {UV_ENOMEM,      "Out of memory"},
    {UV_ENONET,      "Machine is not on the network"},
    {UV_ENOPROTOOPT, "Protocol not available"},
    {UV_ENOSPC,      "No space left on device"},
    {UV_ENOSYS,      "Function not implemented"},
    {UV_ENOTCONN,    "Socket is not connected"},
    {UV_ENOTDIR,     "Not a directory"},
    {UV_ENOTEMPTY,   "Directory not empty"},
    {UV_ENOTSOCK,    "Socket operation on non-socket"},
    {UV_ENOTSUP,     "Operation not supported"},
    {UV_EPERM,       "Operation not permitted"},
    {UV_EPIPE,       "Broken pipe"},
    {UV_EPROTO,      "Protocol error"},
    {UV_EPROTONOSUPPORT, "Protocol not supported"},
    {UV_EPROTOTYPE,  "Protocol wrong type for socket"},
    {UV_ERANGE,      "Result too large"},
    {UV_EROFS,       "Read-only file system"},
    {UV_ESHUTDOWN,   "Cannot send after transport endpoint shutdown"},
    {UV_ESPIPE,      "Invalid seek"},
    {UV_ESRCH,       "No such process"},
    {UV_ETIMEDOUT,   "Connection timed out"},
    {UV_ETXTBSY,     "Text file busy"},
    {UV_EXDEV,       "Cross-device link not permitted"},
    {UV_UNKNOWN,     "Unknown error"},
    {UV_EOF,         "End of file"},
    {UV_ENXIO,       "No such device or address"},
    {UV_EMLINK,      "Too many links"},
    
    // TurboNet-specific errors (using high values to avoid conflicts)
    {TURBO_EINVAL_TRANSPORT, "Invalid transport type in URL"},
    {TURBO_EUNSUPPORTED,     "Unsupported operation"},
    {TURBO_ETLS_HANDSHAKE,   "TLS handshake failed"},
    {TURBO_ETLS_CERT,        "TLS certificate error"},
    {TURBO_EKCP_PROTOCOL,    "KCP protocol error"},
    {TURBO_EQUIC_PROTOCOL,   "QUIC protocol error"},
    
    {0, NULL} // sentinel
};

// Get current timestamp in microseconds
static uint64_t get_timestamp_us(void) {
#ifdef _WIN32
    // Windows high-resolution timestamp
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    
    // Convert FILETIME to 64-bit value (100-nanosecond intervals since Jan 1, 1601)
    uint64_t timestamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    
    // Convert to microseconds since Unix epoch (Jan 1, 1970)
    // Difference between 1601 and 1970 in 100-nanosecond intervals
    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
    timestamp = (timestamp - EPOCH_DIFF) / 10;  // Convert to microseconds
    
    return timestamp;
#else
    // Unix/Linux using clock_gettime
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    }
    return 0;
#endif
}

// Find human-readable message for error code
TURBONET_API const char* turbo_strerror(int code) {
    for (int i = 0; g_error_messages[i].message != NULL; i++) {
        if (g_error_messages[i].code == code) {
            return g_error_messages[i].message;
        }
    }
    
    // Fallback to libuv's message if we don't have it
    const char* uv_msg = uv_strerror(code);
    if (uv_msg && strcmp(uv_msg, "unknown error") != 0) {
        return uv_msg;
    }
    
    return "Unknown error";
}

// =============================================================================
// Public API Implementation
// =============================================================================

TURBONET_API int turbo_get_last_error(turbo_handle_t* handle, turbo_error_t* error) {
    if (!handle || !error) {
        return UV_EINVAL;
    }
    
    // Copy the stored error
    *error = handle->last_error;
    
    // Return 0 if no error, or the error code if there was one
    return error->code == 0 ? 0 : error->code;
}

TURBONET_API void turbo_clear_error(turbo_handle_t* handle) {
    if (!handle) return;
    
    memset(&handle->last_error, 0, sizeof(handle->last_error));
}

TURBONET_API void turbo_set_error(turbo_handle_t* handle, int code, 
                    const char* context, const char* details) {
    if (!handle) return;
    
    turbo_error_t* error = &handle->last_error;
    
    // Clear previous error
    memset(error, 0, sizeof(*error));
    
    // Set error information
    error->code = code;
    error->timestamp = get_timestamp_us();
    
    // Set human-readable message
    const char* base_message = turbo_strerror(code);
    strncpy(error->message, base_message, sizeof(error->message) - 1);
    
    // Set context if provided
    if (context) {
        strncpy(error->context, context, sizeof(error->context) - 1);
    }
    
    // Set technical details if provided
    if (details) {
        strncpy(error->details, details, sizeof(error->details) - 1);
    }
    
    // Log the error for debugging
    if (code != 0) {
        log_error("TurboNet Error: [%d] %s | Context: %s | Details: %s",
                  code, error->message, 
                  error->context[0] ? error->context : "none",
                  error->details[0] ? error->details : "none");
    }
    
    // Call error callback if set (for non-fatal errors)
    if (handle->error_cb && code != 0) {
        handle->error_cb(handle, error);
    }
}

TURBONET_API int turbo_set_error_callback(turbo_handle_t* handle, turbo_error_cb error_cb) {
    if (!handle) {
        return UV_EINVAL;
    }
    
    handle->error_cb = error_cb;
    return 0;
}

TURBONET_API int turbo_error_to_string(const turbo_error_t* error, char* buffer, size_t buffer_size) {
    if (!error || !buffer || buffer_size == 0) {
        return UV_EINVAL;
    }
    
    // Format: "[ERROR_CODE] Message | Context: context | Details: details"
    int written = snprintf(buffer, buffer_size,
        "[%d] %s%s%s%s%s",
        error->code,
        error->message,
        error->context[0] ? " | Context: " : "",
        error->context[0] ? error->context : "",
        error->details[0] ? " | Details: " : "",
        error->details[0] ? error->details : ""
    );
    
    return (written >= 0 && (size_t)written < buffer_size) ? 0 : UV_ENOBUFS;
}

// =============================================================================
// Internal helper functions for transport implementations
// =============================================================================

void turbo_set_error_with_errno(turbo_handle_t* handle, int code, 
                                const char* context, int errno_val) {
    char details[128];
    if (errno_val != 0) {
        snprintf(details, sizeof(details), "System errno: %d (%s)", 
                errno_val, strerror(errno_val));
    } else {
        details[0] = '\0';
    }
    
    turbo_set_error(handle, code, context, details);
}

void turbo_set_tls_error(turbo_handle_t* handle, int code, 
                        const char* context, unsigned long ssl_error) {
    char details[128];
    if (ssl_error != 0) {
#ifdef HAVE_OPENSSL
        // Get OpenSSL error string
        char ssl_buf[128];
        ERR_error_string_n(ssl_error, ssl_buf, sizeof(ssl_buf));
        snprintf(details, sizeof(details), "SSL error: 0x%lx (%s)", 
                ssl_error, ssl_buf);
#else
        snprintf(details, sizeof(details), "SSL error: 0x%lx", ssl_error);
#endif
    } else {
        strcpy(details, "SSL handshake failed");
    }
    
    turbo_set_error(handle, code, context, details);
}

void turbo_set_dns_error(turbo_handle_t* handle, int code, 
                        const char* hostname, const char* error_details) {
    char context[128];
    snprintf(context, sizeof(context), "DNS resolution for '%s'", 
             hostname ? hostname : "unknown");
    
    turbo_set_error(handle, code, context, error_details);
}
