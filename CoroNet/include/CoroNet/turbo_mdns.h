#ifndef TURBONET_MDNS_H
#define TURBONET_MDNS_H


#include "coronet_api.h"
#include <platform.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_MAX_NAME_LEN 256
#define MDNS_MAX_TXT_LEN 1024
#define MDNS_MAX_SERVICES 8
#define MDNS_MCAST_ADDR "224.0.0.251"
#define MDNS_PORT 5353

typedef struct mdns_ctx mdns_ctx_t;

typedef struct {
  char instance[MDNS_MAX_NAME_LEN];
  char service_type[MDNS_MAX_NAME_LEN];
  char hostname[MDNS_MAX_NAME_LEN];
  char ip[16];
  uint16_t port;
  uint8_t txt_data[MDNS_MAX_TXT_LEN];
  size_t txt_len;
  uint32_t ttl;
} mdns_service_t;

typedef void (*mdns_discover_cb)(const mdns_service_t *service, void *userdata);

/**
 * @brief Creates a new mDNS context.
 *
 * @param loop Event loop to associate with this mDNS context (opaque pointer).
 * @return A pointer to the newly created `mdns_ctx_t` instance, or NULL on failure.
 */
CORONET_C_API mdns_ctx_t *mdns_create(void *loop);
/**
 * @brief Destroys an mDNS context and frees associated resources.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance to destroy.
 */
CORONET_C_API void mdns_destroy(mdns_ctx_t *ctx);

/**
 * @brief Publishes an mDNS service, making it discoverable on the local network.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance.
 * @param service A pointer to an `mdns_service_t` structure containing the service details.
 * @return 0 on success, or a non-zero error code on failure.
 */
CORONET_C_API int mdns_publish(mdns_ctx_t *ctx, const mdns_service_t *service);
/**
 * @brief Publishes multiple mDNS services on the same context.
 *
 * Existing matching entries are updated in place; new entries are appended.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance.
 * @param services Array of services to publish.
 * @param count Number of services in @p services.
 * @return 0 on success, or a non-zero error code on failure.
 */
CORONET_C_API int mdns_publish_many(mdns_ctx_t *ctx, const mdns_service_t *services, size_t count);
/**
 * @brief Unpublishes an mDNS service.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance.
 * @param instance The instance name of the service to unpublish.
 * @param service_type The service type (e.g., "_http._tcp") of the service to unpublish.
 * @return 0 on success, or a non-zero error code on failure.
 */
CORONET_C_API int mdns_unpublish(mdns_ctx_t *ctx, const char *instance, const char *service_type);
/**
 * @brief Unpublishes all services currently published by this context.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance.
 * @return 0 on success, or a non-zero error code on failure.
 */
CORONET_C_API int mdns_unpublish_all(mdns_ctx_t *ctx);

/**
 * @brief Initiates mDNS service discovery for a specified service type.
 *        Discovered services are reported via the provided callback.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance.
 * @param service_type The service type to discover (e.g., "_http._tcp").
 * @param callback The callback function to be invoked when a service is discovered.
 * @param userdata User-defined data to be passed to the callback.
 * @param timeout_ms The duration in milliseconds to perform discovery. 0 for continuous discovery.
 * @return 0 on success (discovery initiated), or a non-zero error code on failure.
 */
CORONET_C_API int mdns_discover(mdns_ctx_t *ctx, const char *service_type, mdns_discover_cb callback,
                            void *userdata, uint32_t timeout_ms);
/**
 * @brief Initiates discovery for multiple service types at once.
 *
 * @param ctx A pointer to the `mdns_ctx_t` instance.
 * @param service_types Array of service type strings such as "_http._tcp".
 * @param count Number of entries in @p service_types.
 * @param callback The callback function to be invoked when a service is discovered.
 * @param userdata User-defined data to be passed to the callback.
 * @param timeout_ms The duration in milliseconds to perform discovery. 0 for continuous discovery.
 * @return 0 on success, or a non-zero error code on failure.
 */
CORONET_C_API int mdns_discover_many(mdns_ctx_t *ctx, const char *const *service_types, size_t count,
                                 mdns_discover_cb callback, void *userdata, uint32_t timeout_ms);

/**
 * @brief Retrieves the local hostname.
 *
 * @return A string containing the local hostname.
 */
CORONET_C_API const char *mdns_get_local_hostname(void);
/**
 * @brief Retrieves the local IP address.
 *
 * @return A string containing the local IP address.
 */
CORONET_C_API const char *mdns_get_local_ip(void);

#ifdef __cplusplus
}
#endif

#endif
