#ifndef COOKIE_H
#define COOKIE_H

#include "router.h"
#include "platform.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int max_age;
    char *path;
    char *same_site; // "Strict", "Lax", "None"
    bool http_only;
    bool secure;
} cookie_options_t;

CXX_C_API char *get_cookie(Req *req, const char *name);
CXX_C_API void set_cookie(Res *res, const char *name, const char *value, cookie_options_t *options);

#ifdef __cplusplus
}
#endif

#endif
