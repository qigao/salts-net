#ifndef NETCORE_CLIENT_COMMON_H
#define NETCORE_CLIENT_COMMON_H

#include "turbo_protocol.h"
#include <platform.h>
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
/* Shared client status codes */
typedef enum {
  TURBO_CLIENT_STATUS_OK = 0,
  TURBO_CLIENT_STATUS_INVALID_PARAM,
  TURBO_CLIENT_STATUS_ALLOC_FAILED,
  TURBO_CLIENT_STATUS_NOT_READY,
  TURBO_CLIENT_STATUS_SHUTTING_DOWN,
  TURBO_CLIENT_STATUS_IO_ERROR,
  TURBO_CLIENT_STATUS_TRANSPORT_ERROR,
  TURBO_CLIENT_STATUS_INTERNAL_ERROR
} turbo_client_status_t;

/* Generic transport identifier alias */
typedef turbo_protocol_type_t turbo_client_transport_t;

/**
 * @brief Duplicates a string using memory allocated by the client common module.
 *
 * @param src The source string to duplicate.
 * @return A pointer to the newly allocated and duplicated string, or NULL on allocation failure.
 */
CXX_C_API char *client_common_strdup(const char *src);
/**
 * @brief Resolves a hostname and port into a socket address structure (IP addresses only).
 *
 * This function only handles direct IP address parsing (IPv4/IPv6).
 * For DNS resolution of hostnames, use client_common_resolve_address_with_loop().
 *
 * @param host The IP address to parse.
 * @param port The port number.
 * @param out A pointer to a `struct sockaddr_storage` to store the resolved address.
 * @param out_len A pointer to an integer to store the length of the resolved address.
 * @return 0 on success, or a non-zero error code on failure.
 */
CXX_C_API int client_common_resolve_address(const char *host, int port,
                                            struct sockaddr_storage *out, int *out_len);

/**
 * @brief Acquires a reference to the client common configuration.
 *        This function is typically used to manage the lifecycle of shared resources.
 *
 * @return The current reference count after acquisition.
 */
CXX_C_API int client_common_config_acquire(void);
/**
 * @brief Releases a reference to the client common configuration.
 *        When the reference count drops to zero, shared resources may be deallocated.
 */
CXX_C_API void client_common_config_release(void);

#ifdef __cplusplus
}
#endif

#endif /* NETCORE_CLIENT_COMMON_H */

/* Internal macro - requires <uv.h> to be included first */
#ifdef UV_VERSION_MAJOR
#define CLIENT_COMMON_DEFINE_PIPE_CLIENT_LIST(prefix, type)                                        \
  static uv_once_t g_##prefix##_client_list_once = UV_ONCE_INIT;                                   \
  static uv_mutex_t g_##prefix##_client_list_lock;                                                 \
  static int g_##prefix##_client_list_initialized = 0;                                             \
  static type *g_##prefix##_client_list_head = NULL;
#endif
