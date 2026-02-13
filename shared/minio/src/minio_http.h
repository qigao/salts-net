#ifndef SRC_MINIO_HTTP_H
#define SRC_MINIO_HTTP_H

#include "minio/minio_multimap.h"
#include "minio/minio_error.h"
#include "minio/minio_client.h"
#include <http_client.h>

typedef struct {
    const char* method;
    const char* url;
    MinioHeaders headers;
    const char* body;
    size_t body_len;
} minio_http_request_t;

typedef struct {
    int status_code;
    tstr_t body;
    MinioHeaders headers;
    minio_error_t error;
} minio_http_response_t;

minio_http_response_t minio_http_execute(minio_http_request_t* req);
void minio_http_response_free(minio_http_response_t* resp);

// Build full URL from base_url + path (handles port, scheme)
tstr_t minio_build_url(const minio_base_url_t* base, const char* path);

// Complete sign + execute flow. Caller owns the response.
// extra_headers and query_params are borrowed (not freed by this function).
minio_http_response_t minio_execute_signed(minio_client_t* client,
                                            const char* method,
                                            const char* uri_path,
                                            MinioHeaders* extra_headers,
                                            MinioHeaders* query_params,
                                            const char* body, size_t body_len);

#endif // SRC_MINIO_HTTP_H
