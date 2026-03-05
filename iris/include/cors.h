#ifndef CORS_H
#define CORS_H

#include "router.h"
#include "platform.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char *origin;
    char *methods;
    char *headers;
    char *credentials;
    char *max_age;
    bool enabled;
    bool allow_all_origins; // For "*" support
} cors_t;

CXX_C_API void cors_register(cors_t *opts);
CXX_C_API void reset_cors(void);
CXX_C_API bool cors_handle_preflight(cors_t *opts, const http_context_t *ctx, Res *res);
CXX_C_API void cors_add_headers(cors_t *opts, const http_context_t *ctx, Res *res);
CXX_C_API void init_cors(cors_t *opts);

#ifdef __cplusplus
}
#endif

#endif
