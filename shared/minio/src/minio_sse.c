#include "minio/minio_sse.h"
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <stdlib.h>
#include <string.h>

static tstr_t base64_decode_raw(const char* b64, size_t* out_len) {
    BIO* bio = BIO_new_mem_buf(b64, -1);
    BIO* b64_bio = BIO_new(BIO_f_base64());
    BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64_bio, bio);

    size_t b64_len = strlen(b64);
    char* buf = malloc(b64_len);
    int len = BIO_read(bio, buf, (int)b64_len);
    BIO_free_all(bio);

    if (len < 0) { free(buf); *out_len = 0; return tstr_new(); }
    *out_len = (size_t)len;
    tstr_t result = tstr_dup_len(buf, (size_t)len);
    free(buf);
    return result;
}

static tstr_t md5_base64(const char* data, size_t len) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, md, &md_len);
    EVP_MD_CTX_free(ctx);

    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_write(b64, md, (int)md_len);
    BIO_flush(b64);

    BUF_MEM* bptr;
    BIO_get_mem_ptr(b64, &bptr);
    tstr_t result = tstr_dup_len(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

minio_sse_t minio_sse_s3(void) {
    minio_sse_t sse = {0};
    sse.type = MINIO_SSE_S3;
    return sse;
}

minio_sse_t minio_sse_kms(const char* key_id, const char* context) {
    minio_sse_t sse = {0};
    sse.type = MINIO_SSE_KMS;
    sse.kms_key_id = key_id ? tstr_dup(key_id) : NULL;
    sse.kms_context = context ? tstr_dup(context) : NULL;
    return sse;
}

minio_sse_t minio_sse_customer_key(const char* key_b64) {
    minio_sse_t sse = {0};
    sse.type = MINIO_SSE_C;
    sse.customer_key_b64 = tstr_dup(key_b64);

    // Compute MD5 of the raw key bytes
    size_t raw_len = 0;
    tstr_t raw = base64_decode_raw(key_b64, &raw_len);
    sse.customer_key_md5 = md5_base64(raw, raw_len);
    tstr_free(raw);
    return sse;
}

void minio_sse_free(minio_sse_t* sse) {
    if (!sse) return;
    tstr_free(sse->kms_key_id);
    tstr_free(sse->kms_context);
    tstr_free(sse->customer_key_b64);
    tstr_free(sse->customer_key_md5);
    memset(sse, 0, sizeof(minio_sse_t));
}

void minio_sse_apply_headers(const minio_sse_t* sse, MinioHeaders* headers) {
    if (!sse || !headers) return;
    switch (sse->type) {
        case MINIO_SSE_S3:
            minio_headers_add(headers, "x-amz-server-side-encryption", "AES256");
            break;
        case MINIO_SSE_KMS:
            minio_headers_add(headers, "x-amz-server-side-encryption", "aws:kms");
            if (sse->kms_key_id)
                minio_headers_add(headers, "x-amz-server-side-encryption-aws-kms-key-id", sse->kms_key_id);
            if (sse->kms_context)
                minio_headers_add(headers, "x-amz-server-side-encryption-context", sse->kms_context);
            break;
        case MINIO_SSE_C:
            minio_headers_add(headers, "x-amz-server-side-encryption-customer-algorithm", "AES256");
            minio_headers_add(headers, "x-amz-server-side-encryption-customer-key", sse->customer_key_b64);
            minio_headers_add(headers, "x-amz-server-side-encryption-customer-key-MD5", sse->customer_key_md5);
            break;
        default:
            break;
    }
}

void minio_sse_apply_copy_headers(const minio_sse_t* sse, MinioHeaders* headers) {
    if (!sse || !headers) return;
    if (sse->type == MINIO_SSE_C) {
        minio_headers_add(headers, "x-amz-copy-source-server-side-encryption-customer-algorithm", "AES256");
        minio_headers_add(headers, "x-amz-copy-source-server-side-encryption-customer-key", sse->customer_key_b64);
        minio_headers_add(headers, "x-amz-copy-source-server-side-encryption-customer-key-MD5", sse->customer_key_md5);
    }
}
