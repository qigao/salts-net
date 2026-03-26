#include "s3/s3_signer.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <fmt.h>
#include <string.h>
#include <stc/cstr.h>

static void hex_encode(const unsigned char* data, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) {
        fmt(out + (i * 2), 3, "{:02x}", data[i]);
    }
    out[len * 2] = '\0';
}

tstr_t s3_signer_sha256_hex(const char* data, size_t len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    char hex[EVP_MAX_MD_SIZE * 2 + 1];
    hex_encode(hash, hash_len, hex);
    return tstr_dup(hex);
}

static tstr_t hmac_sha256(const char* key, size_t key_len, const char* data, size_t data_len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    HMAC(EVP_sha256(), key, (int)key_len, (const unsigned char*)data, data_len, hash, &hash_len);
    return tstr_dup_len((const char*)hash, hash_len);
}

s3_error_t s3_signer_sign_v4(const char* service_name, 
                                 const char* method, 
                                 const char* uri, 
                                 const char* region, 
                                 S3Headers* headers, 
                                 S3Headers* query_params,
                                 const char* access_key, 
                                 const char* secret_key, 
                                 const char* session_token,
                                 const char* content_sha256, 
                                 time_t date) {
    if (!headers || !access_key || !secret_key) return s3_error_make(-1, "Invalid params for signing");

    tstr_t amz_date = s3_time_to_amz_date(date);
    tstr_t signer_date = s3_time_to_signer_date(date);

    s3_headers_add(headers, "x-amz-date", amz_date);
    s3_headers_add(headers, "x-amz-content-sha256", content_sha256);
    if (session_token) {
        s3_headers_add(headers, "x-amz-security-token", session_token);
    }

    tstr_t scope = tstr_new();
    scope = tstr_cat_fmt(scope, "%s/%s/%s/aws4_request", signer_date, region, service_name);

    tstr_t signed_headers = NULL;
    tstr_t canonical_headers = NULL;
    s3_headers_get_canonical(headers, &signed_headers, &canonical_headers);

    tstr_t canonical_query = s3_headers_get_canonical_query(query_params);

    tstr_t canonical_request = tstr_new();
    canonical_request = tstr_cat_fmt(canonical_request, "%s\n%s\n%s\n%s\n%s\n%s", 
                                   method, uri, canonical_query, canonical_headers, 
                                   signed_headers, content_sha256);

    tstr_t canonical_request_hash = s3_signer_sha256_hex(canonical_request, tstr_len(canonical_request));

    tstr_t string_to_sign = tstr_new();
    string_to_sign = tstr_cat_fmt(string_to_sign, "AWS4-HMAC-SHA256\n%s\n%s\n%s", 
                                amz_date, scope, canonical_request_hash);

    // Get Signing Key
    tstr_t k_secret = tstr_new();
    k_secret = tstr_cat(k_secret, "AWS4");
    k_secret = tstr_cat(k_secret, secret_key);

    tstr_t k_date = hmac_sha256(k_secret, tstr_len(k_secret), signer_date, tstr_len(signer_date));
    tstr_t k_region = hmac_sha256(k_date, tstr_len(k_date), region, strlen(region));
    tstr_t k_service = hmac_sha256(k_region, tstr_len(k_region), service_name, strlen(service_name));
    tstr_t k_signing = hmac_sha256(k_service, tstr_len(k_service), "aws4_request", 12);

    tstr_t signature_raw = hmac_sha256(k_signing, tstr_len(k_signing), string_to_sign, tstr_len(string_to_sign));
    char signature_hex[EVP_MAX_MD_SIZE * 2 + 1];
    hex_encode((const unsigned char*)signature_raw, tstr_len(signature_raw), signature_hex);

    tstr_t authorization = tstr_new();
    authorization = tstr_cat_fmt(authorization, "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
                               access_key, scope, signed_headers, signature_hex);

    s3_headers_add(headers, "Authorization", authorization);

    // Cleanup
    tstr_free(amz_date); tstr_free(signer_date); tstr_free(scope);
    tstr_free(signed_headers); tstr_free(canonical_headers);
    tstr_free(canonical_query); tstr_free(canonical_request);
    tstr_free(canonical_request_hash); tstr_free(string_to_sign);
    tstr_free(k_secret); tstr_free(k_date); tstr_free(k_region);
    tstr_free(k_service); tstr_free(k_signing); tstr_free(signature_raw);
    tstr_free(authorization);

    return S3_OK;
}

s3_error_t s3_signer_sign_v4_s3(const char* method, 
                                    const char* uri, 
                                    const char* region, 
                                    S3Headers* headers, 
                                    S3Headers* query_params,
                                    const char* access_key, 
                                    const char* secret_key, 
                                    const char* session_token,
                                    const char* content_sha256, 
                                    time_t date) {
    return s3_signer_sign_v4("s3", method, uri, region, headers, query_params, 
                               access_key, secret_key, session_token, content_sha256, date);
}

s3_error_t s3_signer_sign_v4_sts(const char* method,
                                     const char* uri,
                                     const char* region,
                                     S3Headers* headers,
                                     S3Headers* query_params,
                                     const char* access_key,
                                     const char* secret_key,
                                     const char* session_token,
                                     const char* content_sha256,
                                     time_t date) {
    return s3_signer_sign_v4("sts", method, uri, region, headers, query_params,
                               access_key, secret_key, session_token, content_sha256, date);
}

tstr_t s3_signer_presign_v4(const char* method, const char* url,
                                const char* region,
                                S3Headers* query_params,
                                const char* access_key, const char* secret_key,
                                const char* session_token,
                                time_t date, int expires_secs) {
    if (!method || !url || !region || !access_key || !secret_key) return tstr_new();

    tstr_t amz_date = s3_time_to_amz_date(date);
    tstr_t signer_date = s3_time_to_signer_date(date);

    tstr_t scope = tstr_cat_fmt(tstr_new(), "%s/%s/s3/aws4_request", signer_date, region);

    // Parse host and uri from url
    // url is like "https://host:port/bucket/object"
    const char* p = strstr(url, "://");
    const char* host_start = p ? p + 3 : url;
    const char* path_start = strchr(host_start, '/');
    tstr_t host = path_start ? tstr_dup_len(host_start, (size_t)(path_start - host_start)) : tstr_dup(host_start);
    const char* uri_path = path_start ? path_start : "/";

    // Build query params for presigning
    char expires_str[32];
    fmt(expires_str, sizeof(expires_str), "{}", expires_secs);

    s3_headers_add(query_params, "X-Amz-Algorithm", "AWS4-HMAC-SHA256");
    tstr_t credential = tstr_cat_fmt(tstr_new(), "%s/%s", access_key, scope);
    s3_headers_add(query_params, "X-Amz-Credential", credential);
    s3_headers_add(query_params, "X-Amz-Date", amz_date);
    s3_headers_add(query_params, "X-Amz-Expires", expires_str);
    if (session_token) {
        s3_headers_add(query_params, "X-Amz-Security-Token", session_token);
    }
    s3_headers_add(query_params, "X-Amz-SignedHeaders", "host");

    tstr_t canonical_query = s3_headers_get_canonical_query(query_params);

    // Canonical request for presign uses UNSIGNED-PAYLOAD
    tstr_t canonical_request = tstr_cat_fmt(tstr_new(),
        "%s\n%s\n%s\nhost:%s\n\nhost\nUNSIGNED-PAYLOAD",
        method, uri_path, canonical_query, host);

    tstr_t canonical_request_hash = s3_signer_sha256_hex(canonical_request, tstr_len(canonical_request));

    tstr_t string_to_sign = tstr_cat_fmt(tstr_new(),
        "AWS4-HMAC-SHA256\n%s\n%s\n%s",
        amz_date, scope, canonical_request_hash);

    // Derive signing key
    tstr_t k_secret = tstr_cat_fmt(tstr_new(), "AWS4%s", secret_key);
    tstr_t k_date = hmac_sha256(k_secret, tstr_len(k_secret), signer_date, tstr_len(signer_date));
    tstr_t k_region = hmac_sha256(k_date, tstr_len(k_date), region, strlen(region));
    tstr_t k_service = hmac_sha256(k_region, tstr_len(k_region), "s3", 2);
    tstr_t k_signing = hmac_sha256(k_service, tstr_len(k_service), "aws4_request", 12);

    tstr_t signature_raw = hmac_sha256(k_signing, tstr_len(k_signing), string_to_sign, tstr_len(string_to_sign));
    char signature_hex[EVP_MAX_MD_SIZE * 2 + 1];
    hex_encode((const unsigned char*)signature_raw, tstr_len(signature_raw), signature_hex);

    // Build final URL: base_url + ? + canonical_query + &X-Amz-Signature=xxx
    // We need to reconstruct the URL with all query params
    tstr_t scheme_host = tstr_dup_len(url, (size_t)(path_start ? path_start - url : strlen(url)));
    tstr_t result = tstr_cat_fmt(tstr_new(), "%s%s?%s&X-Amz-Signature=%s",
                                  scheme_host, uri_path, canonical_query, signature_hex);

    tstr_free(amz_date); tstr_free(signer_date); tstr_free(scope);
    tstr_free(host); tstr_free(credential); tstr_free(canonical_query);
    tstr_free(canonical_request); tstr_free(canonical_request_hash);
    tstr_free(string_to_sign);
    tstr_free(k_secret); tstr_free(k_date); tstr_free(k_region);
    tstr_free(k_service); tstr_free(k_signing); tstr_free(signature_raw);
    tstr_free(scheme_host);

    return result;
}

s3_error_t s3_signer_post_presign_v4(const char* region,
                                            const char* access_key, const char* secret_key,
                                            const char* session_token,
                                            time_t date,
                                            const char* policy_base64,
                                            S3Headers* form_data_out) {
    if (!region || !access_key || !secret_key || !policy_base64 || !form_data_out)
        return s3_error_make(-1, "Invalid params for post presign");

    tstr_t amz_date = s3_time_to_amz_date(date);
    tstr_t signer_date = s3_time_to_signer_date(date);
    tstr_t scope = tstr_cat_fmt(tstr_new(), "%s/%s/s3/aws4_request", signer_date, region);
    tstr_t credential = tstr_cat_fmt(tstr_new(), "%s/%s", access_key, scope);

    s3_headers_add(form_data_out, "x-amz-algorithm", "AWS4-HMAC-SHA256");
    s3_headers_add(form_data_out, "x-amz-credential", credential);
    s3_headers_add(form_data_out, "x-amz-date", amz_date);
    s3_headers_add(form_data_out, "policy", policy_base64);
    if (session_token) {
        s3_headers_add(form_data_out, "x-amz-security-token", session_token);
    }

    // Sign the policy
    tstr_t k_secret = tstr_cat_fmt(tstr_new(), "AWS4%s", secret_key);
    tstr_t k_date = hmac_sha256(k_secret, tstr_len(k_secret), signer_date, tstr_len(signer_date));
    tstr_t k_region = hmac_sha256(k_date, tstr_len(k_date), region, strlen(region));
    tstr_t k_service = hmac_sha256(k_region, tstr_len(k_region), "s3", 2);
    tstr_t k_signing = hmac_sha256(k_service, tstr_len(k_service), "aws4_request", 12);

    tstr_t signature_raw = hmac_sha256(k_signing, tstr_len(k_signing), policy_base64, strlen(policy_base64));
    char signature_hex[EVP_MAX_MD_SIZE * 2 + 1];
    hex_encode((const unsigned char*)signature_raw, tstr_len(signature_raw), signature_hex);

    s3_headers_add(form_data_out, "x-amz-signature", signature_hex);

    tstr_free(amz_date); tstr_free(signer_date); tstr_free(scope); tstr_free(credential);
    tstr_free(k_secret); tstr_free(k_date); tstr_free(k_region);
    tstr_free(k_service); tstr_free(k_signing); tstr_free(signature_raw);

    return S3_OK;
}
