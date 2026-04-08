#include "mime_smime.h"
#include "turbo_str.h"
#include <stdio.h>
#include <string.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/x509.h>

/* ── Context structure ─────────────────────────────────────────────── */

struct mime_smime_ctx_s {
  X509 *cert;              // Certificate
  EVP_PKEY *pkey;          // Private key
  char error_msg[256];     // Last error message
};

/* ── Context management ────────────────────────────────────────────── */

mime_smime_ctx_t *mime_smime_ctx_create(void) {
  mime_smime_ctx_t *ctx = (mime_smime_ctx_t *)calloc(1, sizeof(mime_smime_ctx_t));
  if (!ctx) return NULL;

  // Initialize OpenSSL
  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();

  return ctx;
}

void mime_smime_ctx_free(mime_smime_ctx_t *ctx) {
  if (!ctx) return;

  if (ctx->cert) X509_free(ctx->cert);
  if (ctx->pkey) EVP_PKEY_free(ctx->pkey);

  free(ctx);
}

/* ── Certificate/Key loading ───────────────────────────────────────── */

int mime_smime_load_cert(mime_smime_ctx_t *ctx, const char *cert_path) {
  if (!ctx || !cert_path) return MIME_SMIME_ERROR_CERT_LOAD;

  FILE *fp = fopen(cert_path, "r");
  if (!fp) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to open cert file: %s", cert_path);
    return MIME_SMIME_ERROR_CERT_LOAD;
  }

  X509 *cert = PEM_read_X509(fp, NULL, NULL, NULL);
  fclose(fp);

  if (!cert) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to parse certificate");
    return MIME_SMIME_ERROR_CERT_LOAD;
  }

  if (ctx->cert) X509_free(ctx->cert);
  ctx->cert = cert;

  return MIME_SMIME_OK;
}

int mime_smime_load_cert_mem(mime_smime_ctx_t *ctx, const char *cert_pem, size_t len) {
  if (!ctx || !cert_pem) return MIME_SMIME_ERROR_CERT_LOAD;

  BIO *bio = BIO_new_mem_buf(cert_pem, (int)len);
  if (!bio) return MIME_SMIME_ERROR_MEMORY;

  X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);

  if (!cert) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to parse certificate from memory");
    return MIME_SMIME_ERROR_CERT_LOAD;
  }

  if (ctx->cert) X509_free(ctx->cert);
  ctx->cert = cert;

  return MIME_SMIME_OK;
}

int mime_smime_load_key(mime_smime_ctx_t *ctx, const char *key_path, const char *password) {
  if (!ctx || !key_path) return MIME_SMIME_ERROR_KEY_LOAD;

  FILE *fp = fopen(key_path, "r");
  if (!fp) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to open key file: %s", key_path);
    return MIME_SMIME_ERROR_KEY_LOAD;
  }

  EVP_PKEY *pkey = PEM_read_PrivateKey(fp, NULL, NULL, (void *)password);
  fclose(fp);

  if (!pkey) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to parse private key");
    return MIME_SMIME_ERROR_KEY_LOAD;
  }

  if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
  ctx->pkey = pkey;

  return MIME_SMIME_OK;
}

int mime_smime_load_key_mem(mime_smime_ctx_t *ctx, const char *key_pem, size_t len,
                             const char *password) {
  if (!ctx || !key_pem) return MIME_SMIME_ERROR_KEY_LOAD;

  BIO *bio = BIO_new_mem_buf(key_pem, (int)len);
  if (!bio) return MIME_SMIME_ERROR_MEMORY;

  EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, (void *)password);
  BIO_free(bio);

  if (!pkey) {
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Failed to parse private key from memory");
    return MIME_SMIME_ERROR_KEY_LOAD;
  }

  if (ctx->pkey) EVP_PKEY_free(ctx->pkey);
  ctx->pkey = pkey;

  return MIME_SMIME_OK;
}

/* ── Encryption ────────────────────────────────────────────────────── */

char *mime_smime_encrypt(mime_smime_ctx_t *ctx, const char *message, size_t message_len,
                         size_t *output_len, mime_smime_error_t *error) {
  if (!ctx || !ctx->cert || !message || !output_len) {
    if (error) *error = MIME_SMIME_ERROR_ENCRYPT;
    return NULL;
  }

  BIO *in = BIO_new_mem_buf(message, (int)message_len);
  if (!in) {
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  STACK_OF(X509) *recips = sk_X509_new_null();
  sk_X509_push(recips, ctx->cert);

  PKCS7 *p7 = PKCS7_encrypt(recips, in, EVP_aes_256_cbc(), PKCS7_BINARY);
  BIO_free(in);
  sk_X509_free(recips);

  if (!p7) {
    if (error) *error = MIME_SMIME_ERROR_ENCRYPT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "PKCS7_encrypt failed");
    return NULL;
  }

  BIO *out = BIO_new(BIO_s_mem());
  if (!out) {
    PKCS7_free(p7);
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  SMIME_write_PKCS7(out, p7, NULL, PKCS7_BINARY);
  PKCS7_free(p7);

  BUF_MEM *bptr;
  BIO_get_mem_ptr(out, &bptr);

  char *result = (char *)malloc(bptr->length + 1);
  if (!result) {
    BIO_free(out);
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  memcpy(result, bptr->data, bptr->length);
  result[bptr->length] = '\0';
  *output_len = bptr->length;

  BIO_free(out);

  if (error) *error = MIME_SMIME_OK;
  return result;
}

/* ── Decryption ────────────────────────────────────────────────────── */

char *mime_smime_decrypt(mime_smime_ctx_t *ctx, const char *encrypted, size_t encrypted_len,
                         size_t *output_len, mime_smime_error_t *error) {
  if (!ctx || !ctx->cert || !ctx->pkey || !encrypted || !output_len) {
    if (error) *error = MIME_SMIME_ERROR_DECRYPT;
    return NULL;
  }

  BIO *in = BIO_new_mem_buf(encrypted, (int)encrypted_len);
  if (!in) {
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  PKCS7 *p7 = SMIME_read_PKCS7(in, NULL);
  BIO_free(in);

  if (!p7) {
    if (error) *error = MIME_SMIME_ERROR_DECRYPT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "SMIME_read_PKCS7 failed");
    return NULL;
  }

  BIO *out = BIO_new(BIO_s_mem());
  if (!out) {
    PKCS7_free(p7);
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  if (PKCS7_decrypt(p7, ctx->pkey, ctx->cert, out, 0) != 1) {
    PKCS7_free(p7);
    BIO_free(out);
    if (error) *error = MIME_SMIME_ERROR_DECRYPT;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "PKCS7_decrypt failed");
    return NULL;
  }

  PKCS7_free(p7);

  BUF_MEM *bptr;
  BIO_get_mem_ptr(out, &bptr);

  char *result = (char *)malloc(bptr->length + 1);
  if (!result) {
    BIO_free(out);
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  memcpy(result, bptr->data, bptr->length);
  result[bptr->length] = '\0';
  *output_len = bptr->length;

  BIO_free(out);

  if (error) *error = MIME_SMIME_OK;
  return result;
}

/* ── Signing ───────────────────────────────────────────────────────── */

char *mime_smime_sign(mime_smime_ctx_t *ctx, const char *message, size_t message_len,
                      size_t *output_len, mime_smime_error_t *error) {
  if (!ctx || !ctx->cert || !ctx->pkey || !message || !output_len) {
    if (error) *error = MIME_SMIME_ERROR_SIGN;
    return NULL;
  }

  BIO *in = BIO_new_mem_buf(message, (int)message_len);
  if (!in) {
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  PKCS7 *p7 = PKCS7_sign(ctx->cert, ctx->pkey, NULL, in, PKCS7_BINARY | PKCS7_DETACHED);
  BIO_free(in);

  if (!p7) {
    if (error) *error = MIME_SMIME_ERROR_SIGN;
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "PKCS7_sign failed");
    return NULL;
  }

  BIO *out = BIO_new(BIO_s_mem());
  if (!out) {
    PKCS7_free(p7);
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  in = BIO_new_mem_buf(message, (int)message_len);
  SMIME_write_PKCS7(out, p7, in, PKCS7_BINARY | PKCS7_DETACHED);
  BIO_free(in);
  PKCS7_free(p7);

  BUF_MEM *bptr;
  BIO_get_mem_ptr(out, &bptr);

  char *result = (char *)malloc(bptr->length + 1);
  if (!result) {
    BIO_free(out);
    if (error) *error = MIME_SMIME_ERROR_MEMORY;
    return NULL;
  }

  memcpy(result, bptr->data, bptr->length);
  result[bptr->length] = '\0';
  *output_len = bptr->length;

  BIO_free(out);

  if (error) *error = MIME_SMIME_OK;
  return result;
}

/* ── Verification ──────────────────────────────────────────────────── */

int mime_smime_verify(mime_smime_ctx_t *ctx, const char *signed_data, size_t signed_len,
                      char **output, size_t *output_len) {
  if (!ctx || !signed_data) return MIME_SMIME_ERROR_VERIFY;

  BIO *in = BIO_new_mem_buf(signed_data, (int)signed_len);
  if (!in) return MIME_SMIME_ERROR_MEMORY;

  BIO *content = NULL;
  PKCS7 *p7 = SMIME_read_PKCS7(in, &content);
  BIO_free(in);

  if (!p7) {
    if (content) BIO_free(content);
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "SMIME_read_PKCS7 failed");
    return MIME_SMIME_ERROR_VERIFY;
  }

  BIO *out = NULL;
  if (output) {
    out = BIO_new(BIO_s_mem());
    if (!out) {
      PKCS7_free(p7);
      if (content) BIO_free(content);
      return MIME_SMIME_ERROR_MEMORY;
    }
  }

  X509_STORE *store = X509_STORE_new();
  if (ctx->cert) X509_STORE_add_cert(store, ctx->cert);

  int verify_result = PKCS7_verify(p7, NULL, store, content, out, PKCS7_NOVERIFY);

  X509_STORE_free(store);
  PKCS7_free(p7);
  if (content) BIO_free(content);

  if (verify_result != 1) {
    if (out) BIO_free(out);
    snprintf(ctx->error_msg, sizeof(ctx->error_msg), "PKCS7_verify failed");
    return MIME_SMIME_ERROR_VERIFY;
  }

  if (output && out) {
    BUF_MEM *bptr;
    BIO_get_mem_ptr(out, &bptr);

    *output = (char *)malloc(bptr->length + 1);
    if (!*output) {
      BIO_free(out);
      return MIME_SMIME_ERROR_MEMORY;
    }

    memcpy(*output, bptr->data, bptr->length);
    (*output)[bptr->length] = '\0';
    if (output_len) *output_len = bptr->length;
  }

  if (out) BIO_free(out);

  return MIME_SMIME_OK;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

int mime_is_smime_encrypted(const char *content_type, size_t len) {
  if (!content_type || len < 20) return 0;
  return (strstr(content_type, "application/pkcs7-mime") != NULL &&
          strstr(content_type, "smime-type=enveloped-data") != NULL);
}

int mime_is_smime_signed(const char *content_type, size_t len) {
  if (!content_type || len < 20) return 0;
  return (strstr(content_type, "application/pkcs7-mime") != NULL &&
          strstr(content_type, "smime-type=signed-data") != NULL) ||
         (strstr(content_type, "multipart/signed") != NULL &&
          strstr(content_type, "protocol=application/pkcs7-signature") != NULL);
}

const char *mime_smime_get_error(mime_smime_ctx_t *ctx) {
  return ctx ? ctx->error_msg : "Invalid context";
}
