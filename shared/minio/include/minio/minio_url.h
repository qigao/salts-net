#ifndef MINIO_URL_H
#define MINIO_URL_H

#include <turbo_str.h>
#include <turbo_str_view.h>
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     is_https;
    tstr_t  host;
    int     port;
    tstr_t  path;
    tstr_t  query_string;
} minio_url_t;

CXX_C_API minio_url_t minio_url_parse(const char* input);
CXX_C_API tstr_t      minio_url_to_string(const minio_url_t* url);

// Wrappers around TurboNet's URL encoding
CXX_C_API tstr_t      minio_url_encode(const char* input);
CXX_C_API tstr_t      minio_url_decode(const char* input);

CXX_C_API void        minio_url_free(minio_url_t* url);

#ifdef __cplusplus
}
#endif

#endif // MINIO_URL_H
