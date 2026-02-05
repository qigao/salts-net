#ifndef TURBO_DNS_H
#define TURBO_DNS_H

#include <platform.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// Types
// =============================================================================

/**
 * @brief Address family preference for DNS resolution.
 */
typedef enum {
  TURBO_DNS_IPV4_ONLY = 0,   /**< Resolve IPv4 addresses only */
  TURBO_DNS_IPV6_ONLY = 1,   /**< Resolve IPv6 addresses only */
  TURBO_DNS_PREFER_IPV6 = 2, /**< Try IPv6 first, fallback to IPv4 */
  TURBO_DNS_ANY = 3          /**< Return first available (IPv4 or IPv6) */
} turbo_dns_pref_t;

/**
 * @brief Callback for asynchronous DNS resolution.
 *
 * @param hostname The original hostname that was resolved
 * @param ip       The resolved IP address string (NULL on failure)
 * @param status   0 on success, error code on failure
 * @param user_data User-provided context pointer
 */
typedef void (*turbo_dns_cb)(const char *hostname, const char *ip, int status, void *user_data);

// =============================================================================
// Lifecycle
// =============================================================================

/**
 * @brief Initialize the DNS resolver subsystem.
 *
 * Safe to call multiple times (reference counted).
 *
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_dns_init(void);

/**
 * @brief Cleanup the DNS resolver subsystem.
 *
 * Reference counted - only cleans up when all references released.
 */
CXX_C_API void turbo_dns_cleanup(void);

// =============================================================================
// Synchronous API (blocking)
// =============================================================================

/**
 * @brief Resolve hostname to sockaddr (blocking).
 *
 * If input is already an IP address, returns immediately.
 * Otherwise blocks until DNS resolution completes.
 *
 * @param loop    Event loop to use (NULL to create temporary loop)
 * @param host    Hostname or IP address to resolve
 * @param port    Port number for the resulting sockaddr
 * @param out     Output sockaddr_storage
 * @param out_len Output length of the sockaddr
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_dns_resolve(void *loop, const char *host, int port,
                                struct sockaddr_storage *out, int *out_len);

/**
 * @brief Resolve hostname to IP string (blocking).
 *
 * @param hostname    Hostname or IP address to resolve
 * @param ip_buffer   Buffer to store resolved IP string
 * @param buffer_size Size of ip_buffer (INET6_ADDRSTRLEN recommended)
 * @param family_pref 4 for IPv4 only, 6 for IPv6 only, 0 for any
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_dns_resolve_sync(const char *hostname, char *ip_buffer, size_t buffer_size,
                                     int family_pref);

// =============================================================================
// Asynchronous API (callback-based)
// =============================================================================

/**
 * @brief Start asynchronous DNS resolution with family preference.
 *
 * @param loop      Event loop for resolution (opaque pointer)
 * @param hostname  Hostname to resolve
 * @param pref      Address family preference
 * @param callback  Callback invoked on completion
 * @param user_data User context passed to callback
 * @return 0 on success (resolution started), error code on failure
 */
CXX_C_API int turbo_dns_resolve_async(void *loop, const char *hostname, turbo_dns_pref_t pref,
                                      turbo_dns_cb callback, void *user_data);

// =============================================================================
// DNS Server Configuration
// =============================================================================

/**
 * @brief Set custom DNS servers.
 *
 * @param servers Array of DNS server IP addresses (IPv4/IPv6 strings)
 * @param count   Number of servers (max 8)
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_dns_set_servers(const char *servers[], int count);

/**
 * @brief Get currently configured DNS servers.
 *
 * @param servers     Output array for DNS server addresses
 * @param max_servers Maximum servers to retrieve
 * @param count       Output: actual number retrieved
 * @return 0 on success, error code on failure
 */
CXX_C_API int turbo_dns_get_servers(char servers[][46], int max_servers, int *count);

// =============================================================================
// Utilities
// =============================================================================

/**
 * @brief Parse IP address string to sockaddr_storage.
 *
 * Supports both IPv4 and IPv6 addresses.
 *
 * @param address IP address string
 * @param port    Port number
 * @param addr    Output sockaddr_storage
 * @return 0 on success, UV_EAI_NONAME if not a valid IP
 */
CXX_C_API int turbo_dns_parse_address(const char *address, int port, struct sockaddr_storage *addr);

#ifdef __cplusplus
}
#endif

#endif // TURBO_DNS_H
