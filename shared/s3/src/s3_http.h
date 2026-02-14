#ifndef SRC_S3_HTTP_H
#define SRC_S3_HTTP_H

#include "s3/s3_multimap.h"
#include "s3/s3_error.h"
#include "s3/s3_client.h"
#include <http_client.h>

typedef struct {
    const char* method;
    const char* url;
    S3Headers headers;
    const char* body;
    size_t body_len;
} s3_http_request_t;

typedef struct {
    int status_code;
    tstr_t body;
    S3Headers headers;
    s3_error_t error;
} s3_http_response_t;

s3_http_response_t s3_http_execute(s3_http_request_t* req);
void s3_http_response_free(s3_http_response_t* resp);

// Build full URL from base_url + path (handles port, scheme)
tstr_t s3_build_url(const s3_base_url_t* base, const char* path);

// Complete sign + execute flow. Caller owns the response.
// extra_headers and query_params are borrowed (not freed by this function).
s3_http_response_t s3_execute_signed(s3_client_t* client,
                                            const char* method,
                                            const char* uri_path,
                                            S3Headers* extra_headers,
                                            S3Headers* query_params,
                                            const char* body, size_t body_len);

#endif // SRC_S3_HTTP_H
