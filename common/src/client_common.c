#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "client_common.h"

static uv_once_t g_config_guard_once = UV_ONCE_INIT;
static uv_mutex_t g_config_guard_lock;
static int g_config_guard_lock_initialized = 0;
static int g_config_guard_refcount = 0;

/**
 * @brief Initializes the global configuration guard mutex.
 *        This function is called once to ensure thread-safe access to the configuration.
 */
static void client_common_config_init_once(void) {
  if (uv_mutex_init(&g_config_guard_lock) == 0)
    g_config_guard_lock_initialized = 1;
}

/**
 * @brief Duplicates a string using dynamically allocated memory.
 *
 * @param src The source string to duplicate.
 * @return A pointer to the newly allocated and duplicated string, or NULL on allocation failure.
 */
char *client_common_strdup(const char *src) {
  if (!src)
    return NULL;
  size_t len = strlen(src);
  char *copy = (char *)malloc(len + 1);
  if (!copy)
    return NULL;
  memcpy(copy, src, len + 1);
  return copy;
}

/**
 * @brief Resolves a hostname and port into a socket address structure.
 *        Supports both IPv4 and IPv6, with DNS resolution for hostnames.
 *
 * This function first attempts to parse the host as an IP address.
 * If that fails, it performs DNS resolution using c-ares.
 *
 * @param host The hostname or IP address to resolve.
 * @param port The port number.
 * @param out A pointer to a `struct sockaddr_storage` to store the resolved address.
 * @param out_len A pointer to an integer to store the length of the resolved address.
 * @return 0 on success, or a libuv error code on failure.
 *
 * @note This function only handles direct IP address parsing. For DNS resolution,
 *       use client_common_resolve_address_with_loop().
 */
int client_common_resolve_address(const char *host, int port, struct sockaddr_storage *out,
                                  int *out_len) {
  if (!host || !out || !out_len)
    return UV_EINVAL;

  /* Try direct IP address parsing first (fast path) */
  struct sockaddr_in addr4;
  int rc = uv_ip4_addr(host, port, &addr4);
  if (rc == 0) {
    memcpy(out, &addr4, sizeof(addr4));
    *out_len = (int)sizeof(addr4);
    return 0;
  }

  struct sockaddr_in6 addr6;
  rc = uv_ip6_addr(host, port, &addr6);
  if (rc == 0) {
    memcpy(out, &addr6, sizeof(addr6));
    *out_len = (int)sizeof(addr6);
    return 0;
  }

  /* Not an IP address, return error - DNS resolution handled by caller */
  return UV_EINVAL;
}

/**
 * @brief Acquires a reference to the common configuration.
 *        Increments the reference count for resource management.
 *
 * @return The current reference count after acquisition.
 */
int client_common_config_acquire(void) {
  uv_once(&g_config_guard_once, client_common_config_init_once);
  if (!g_config_guard_lock_initialized)
    return 0;

  uv_mutex_lock(&g_config_guard_lock);
  g_config_guard_refcount++;
  int rc = g_config_guard_refcount;
  uv_mutex_unlock(&g_config_guard_lock);
  return rc;
}

/**
 * @brief Releases a reference to the common configuration.
 *        Decrements the reference count.
 */
void client_common_config_release(void) {
  if (!g_config_guard_lock_initialized)
    return;

  uv_mutex_lock(&g_config_guard_lock);
  if (g_config_guard_refcount > 0) {
    g_config_guard_refcount--;
  }
  uv_mutex_unlock(&g_config_guard_lock);
}
