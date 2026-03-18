#ifndef S3_MULTIMAP_H
#define S3_MULTIMAP_H

#include <turbo_str.h>
#include <platform.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque type for S3 headers map (implementation hidden)
typedef struct S3Headers S3Headers;

CXX_C_API void        s3_headers_add(S3Headers* m, const char* key, const char* value);
CXX_C_API const char* s3_headers_get(S3Headers* m, const char* key);
CXX_C_API int         s3_headers_contains(S3Headers* m, const char* key);

// Convert all headers to a query string format
CXX_C_API tstr_t      s3_headers_to_query_string(S3Headers* m);

// Convert headers to array of "Key: Value" strings for TurboNet HTTP client
CXX_C_API const char** s3_headers_to_http_array(S3Headers* m, int* count);

// Helper to get canonical headers and signed headers list for AWS SigV4
CXX_C_API void s3_headers_get_canonical(S3Headers* m, tstr_t* signed_headers, tstr_t* canonical_headers);

// Helper to get canonical query string for AWS SigV4
CXX_C_API tstr_t s3_headers_get_canonical_query(S3Headers* m);

#ifdef __cplusplus
}
#endif

#endif // S3_MULTIMAP_H
