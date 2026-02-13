#ifndef MINIO_MULTIMAP_H
#define MINIO_MULTIMAP_H

#include <turbo_str.h>
#include <platform.h>
#include <stc/cstr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Define MinioHeaders as a map of cstr to cstr
// NOTE: S3 occasionally allows multiple values for the same header (e.g. Set-Cookie),
// but for request headers and most response headers, a unique map is sufficient.
// If multi-values are needed, we can switch to a vector of pairs or STC multimap.
#define i_static
#define i_type MinioHeaders
#define i_key_str
#define i_val_str
#include <stc/hmap.h>

CXX_C_API void        minio_headers_add(MinioHeaders* m, const char* key, const char* value);
CXX_C_API const char* minio_headers_get(MinioHeaders* m, const char* key);
CXX_C_API int         minio_headers_contains(MinioHeaders* m, const char* key);

// Convert all headers to a query string format
CXX_C_API tstr_t      minio_headers_to_query_string(MinioHeaders* m);

// Convert headers to array of "Key: Value" strings for TurboNet HTTP client
CXX_C_API const char** minio_headers_to_http_array(MinioHeaders* m, int* count);

// Helper to get canonical headers and signed headers list for AWS SigV4
CXX_C_API void minio_headers_get_canonical(MinioHeaders* m, tstr_t* signed_headers, tstr_t* canonical_headers);

// Helper to get canonical query string for AWS SigV4
CXX_C_API tstr_t minio_headers_get_canonical_query(MinioHeaders* m);

#ifdef __cplusplus
}
#endif

#endif // MINIO_MULTIMAP_H
