#ifndef S3_SIGNER_H
#define S3_SIGNER_H

#include <turbo_str.h>
#include "s3_error.h"
#include "s3_multimap.h"
#include "s3_time.h"
#include <http_client.h>

#ifdef __cplusplus
extern "C" {
#endif

// Signs S3 requests with AWS Signature V4
CXX_C_API s3_error_t s3_signer_sign_v4(const char* service_name,
                                 const char* method,
                                 const char* uri,
                                 const char* region,
                                 S3Headers* headers,
                                 S3Headers* query_params,
                                 const char* access_key,
                                 const char* secret_key,
                                 const char* session_token,
                                 const char* content_sha256,
                                 time_t date);

// Specifically for S3 (service_name="s3")
CXX_C_API s3_error_t s3_signer_sign_v4_s3(const char* method,
                                    const char* uri,
                                    const char* region,
                                    S3Headers* headers,
                                    S3Headers* query_params,
                                    const char* access_key,
                                    const char* secret_key,
                                    const char* session_token,
                                    const char* content_sha256,
                                    time_t date);

// Specifically for STS (service_name="sts")
CXX_C_API s3_error_t s3_signer_sign_v4_sts(const char* method,
                                     const char* uri,
                                     const char* region,
                                     S3Headers* headers,
                                     S3Headers* query_params,
                                     const char* access_key,
                                     const char* secret_key,
                                     const char* session_token,
                                     const char* content_sha256,
                                     time_t date);

// Helper to calculate SHA256 hex string of content
CXX_C_API tstr_t s3_signer_sha256_hex(const char* data, size_t len);

// Presigned URL (GET/PUT) — returns the full presigned URL
CXX_C_API tstr_t s3_signer_presign_v4(const char* method, const char* url,
                                const char* region,
                                S3Headers* query_params,
                                const char* access_key, const char* secret_key,
                                const char* session_token,
                                time_t date, int expires_secs);

// Presigned POST form data — fills form_data_out with required fields
CXX_C_API s3_error_t s3_signer_post_presign_v4(const char* region,
                                            const char* access_key, const char* secret_key,
                                            const char* session_token,
                                            time_t date,
                                            const char* policy_base64,
                                            S3Headers* form_data_out);

#ifdef __cplusplus
}
#endif

#endif // S3_SIGNER_H
