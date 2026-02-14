#ifndef S3_URL_H
#define S3_URL_H

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
} s3_url_t;

CXX_C_API s3_url_t s3_url_parse(const char* input);
CXX_C_API tstr_t      s3_url_to_string(const s3_url_t* url);

// Wrappers around TurboNet's URL encoding
CXX_C_API tstr_t      s3_url_encode(const char* input);
CXX_C_API tstr_t      s3_url_decode(const char* input);

CXX_C_API void        s3_url_free(s3_url_t* url);

#ifdef __cplusplus
}
#endif

#endif // S3_URL_H
