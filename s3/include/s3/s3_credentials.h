#ifndef S3_CREDENTIALS_H
#define S3_CREDENTIALS_H

#include <turbo_str.h>
#include <platform.h>
#include <netcore/turbo_coro_context.h>
#include <time.h>
#include "s3_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    tstr_t access_key;
    tstr_t secret_key;
    tstr_t session_token;
    time_t expiration;       // 0 = no expiration
} s3_credentials_t;

// Fetches credentials, returns S3_OK or error.
typedef s3_error_t (*s3_credentials_fetch_fn)(void* ctx, s3_credentials_t* out);

typedef struct {
    s3_credentials_fetch_fn fetch;
    void* ctx;
    void (*destroy)(void* ctx);
} s3_credential_provider_t;

// Provider factory functions
CXX_C_API s3_credential_provider_t* s3_creds_static(const char* access_key, const char* secret_key, const char* session_token);
CXX_C_API s3_credential_provider_t* s3_creds_env_aws(void);
CXX_C_API s3_credential_provider_t* s3_creds_env_minio(void);
CXX_C_API s3_credential_provider_t* s3_creds_aws_config(const char* filename, const char* profile);
CXX_C_API s3_credential_provider_t* s3_creds_chain(s3_credential_provider_t** providers, int count);

// STS / IAM providers
CXX_C_API s3_credential_provider_t* s3_creds_minio_client_config(const char* filename, const char* alias);
CXX_C_API s3_credential_provider_t* s3_creds_assume_role(coro_context_t *ctx,
                                                      const char* sts_endpoint, const char* access_key,
                                                      const char* secret_key, const char* region,
                                                      const char* role_arn, const char* session_name,
                                                      int duration_secs);
CXX_C_API s3_credential_provider_t* s3_creds_web_identity(coro_context_t *ctx,
                                                       const char* sts_endpoint, const char* region,
                                                       const char* role_arn,
                                                       tstr_t (*token_fn)(void* ctx), void* token_ctx);
CXX_C_API s3_credential_provider_t* s3_creds_iam_aws(coro_context_t *ctx,
                                                      const char* custom_endpoint);
CXX_C_API s3_credential_provider_t* s3_creds_ldap_identity(coro_context_t *ctx,
                                                        const char* sts_endpoint,
                                                        const char* ldap_username, const char* ldap_password);
CXX_C_API s3_credential_provider_t* s3_creds_cert_identity(coro_context_t *ctx,
                                                        const char* sts_endpoint,
                                                        const char* cert_file, const char* key_file);

// Helper to free a credentials struct (clears tstr_t fields)
CXX_C_API void s3_credentials_clear(s3_credentials_t* creds);

// Helper to destroy a provider
CXX_C_API void s3_credential_provider_destroy(s3_credential_provider_t* provider);

#ifdef __cplusplus
}
#endif

#endif // S3_CREDENTIALS_H
