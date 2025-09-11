#ifndef TURBONET_MDNS_H
#define TURBONET_MDNS_H

#include <stdint.h>
#include <stddef.h>
#include <uv.h>

#ifdef _WIN32
    #ifdef TURBONET_BUILDING_DLL
        #define MDNS_API __declspec(dllexport)
    #elif defined(TURBONET_USING_DLL)
        #define MDNS_API __declspec(dllimport)
    #else
        #define MDNS_API
    #endif
#else
    #define MDNS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_MAX_NAME_LEN 256
#define MDNS_MAX_TXT_LEN 1024
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

typedef void (*mdns_discover_cb)(const mdns_service_t* service, void* userdata);

MDNS_API mdns_ctx_t* mdns_create(uv_loop_t* loop);
MDNS_API void mdns_destroy(mdns_ctx_t* ctx);

MDNS_API int mdns_publish(mdns_ctx_t* ctx, const mdns_service_t* service);
MDNS_API int mdns_unpublish(mdns_ctx_t* ctx, const char* instance, const char* service_type);

MDNS_API int mdns_discover(mdns_ctx_t* ctx, const char* service_type, 
                  mdns_discover_cb callback, void* userdata, uint32_t timeout_ms);

MDNS_API const char* mdns_get_local_hostname(void);
MDNS_API const char* mdns_get_local_ip(void);

#ifdef __cplusplus
}
#endif

#endif