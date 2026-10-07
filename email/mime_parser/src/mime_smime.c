#include "mime_smime.h"

#include "asn1_types.h"
#include "base64_utils.h"
#include "cmeta_simd_scan.h"

#include <openssl/evp.h>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIME_SMIME_AES_KEY_SIZE 32U
#define MIME_SMIME_AES_IV_SIZE 16U
#define MIME_SMIME_SHA256_SIZE 32U
#define MIME_SMIME_BOUNDARY "saltsnet-smime-boundary"

#define OID_CMS_DATA "1.2.840.113549.1.7.1"
#define OID_CMS_SIGNED_DATA "1.2.840.113549.1.7.2"
#define OID_CMS_ENVELOPED_DATA "1.2.840.113549.1.7.3"
#define OID_RSA_ENCRYPTION "1.2.840.113549.1.1.1"
#define OID_SHA256 "2.16.840.1.101.3.4.2.1"
#define OID_AES256_CBC "2.16.840.1.101.3.4.1.42"

struct mime_smime_ctx_s {
  X509 *cert;
  EVP_PKEY *pkey;
  char error_msg[256];
};

static void set_error(mime_smime_ctx_t *ctx, const char *message) {
  if (ctx) snprintf(ctx->error_msg, sizeof(ctx->error_msg), "%s", message);
}

static asn1_value_t *context_value(uint32_t tag, int constructed,
                                   const uint8_t *data, size_t data_len) {
  asn1_value_t *value = asn1_create_value(TK_CONTEXT_SPECIFIC, 2,
                                          constructed ? 1 : 0, tag);
  if (!value) return NULL;
  if (!constructed && data_len != 0) {
    value->value.octet_string.data = (uint8_t *)malloc(data_len);
    if (!value->value.octet_string.data) {
      asn1_free(value);
      return NULL;
    }
    memcpy(value->value.octet_string.data, data, data_len);
    value->value.octet_string.length = data_len;
  }
  return value;
}

static int add_sequence_child(asn1_value_t *parent, asn1_value_t *child) {
  if (!child || asn1_sequence_add_child(parent, child) != 0) {
    asn1_free(child);
    return -1;
  }
  return 0;
}

static int add_set_child(asn1_value_t *parent, asn1_value_t *child) {
  if (!child || asn1_set_add_child(parent, child) != 0) {
    asn1_free(child);
    return -1;
  }
  return 0;
}

static asn1_value_t *algorithm_identifier(const char *oid,
                                          const uint8_t *parameter,
                                          size_t parameter_len) {
  asn1_value_t *algorithm = asn1_create_sequence();
  if (!algorithm ||
      add_sequence_child(algorithm, asn1_create_oid_from_string(oid)) != 0) {
    asn1_free(algorithm);
    return NULL;
  }
  if (parameter) {
    if (add_sequence_child(
            algorithm,
            asn1_create_octet_string(parameter, parameter_len)) != 0) {
      asn1_free(algorithm);
      return NULL;
    }
  } else if (add_sequence_child(algorithm, asn1_create_null()) != 0) {
    asn1_free(algorithm);
    return NULL;
  }
  return algorithm;
}

static int encode_der(const asn1_value_t *value, uint8_t **output,
                      size_t *output_len) {
  size_t required = 0;
  uint8_t *buffer;
  if (!value || !output || !output_len ||
      asn1_der_encode(value, NULL, &required) != 0 || required == 0) {
    return -1;
  }
  buffer = (uint8_t *)malloc(required);
  if (!buffer) return -1;
  if (asn1_der_encode(value, buffer, &required) != 0) {
    free(buffer);
    return -1;
  }
  *output = buffer;
  *output_len = required;
  return 0;
}

static int oid_equals(const asn1_value_t *value, const char *expected) {
  char *text;
  int equal;
  if (!value || value->type != ASN1_TYPE_OBJECT_IDENTIFIER) return 0;
  text = asn1_oid_to_string(&value->value.oid);
  if (!text) return 0;
  equal = strcmp(text, expected) == 0;
  free(text);
  return equal;
}

static int encrypt_aes256_cbc(const uint8_t *plaintext, size_t plaintext_len,
                              const uint8_t key[MIME_SMIME_AES_KEY_SIZE],
                              const uint8_t iv[MIME_SMIME_AES_IV_SIZE],
                              uint8_t **ciphertext, size_t *ciphertext_len) {
  EVP_CIPHER_CTX *cipher = NULL;
  uint8_t *buffer = NULL;
  int first_len = 0;
  int final_len = 0;
  int ok = -1;

  if (plaintext_len > (size_t)INT_MAX) return -1;
  buffer = (uint8_t *)malloc(plaintext_len + MIME_SMIME_AES_IV_SIZE);
  cipher = EVP_CIPHER_CTX_new();
  if (!buffer || !cipher) goto cleanup;
  if (EVP_EncryptInit_ex(cipher, EVP_aes_256_cbc(), NULL, key, iv) != 1 ||
      EVP_EncryptUpdate(cipher, buffer, &first_len, plaintext,
                        (int)plaintext_len) != 1 ||
      EVP_EncryptFinal_ex(cipher, buffer + first_len, &final_len) != 1) {
    goto cleanup;
  }
  *ciphertext = buffer;
  *ciphertext_len = (size_t)(first_len + final_len);
  buffer = NULL;
  ok = 0;

cleanup:
  EVP_CIPHER_CTX_free(cipher);
  free(buffer);
  return ok;
}

static int decrypt_aes256_cbc(const uint8_t *ciphertext, size_t ciphertext_len,
                              const uint8_t key[MIME_SMIME_AES_KEY_SIZE],
                              const uint8_t iv[MIME_SMIME_AES_IV_SIZE],
                              uint8_t **plaintext, size_t *plaintext_len) {
  EVP_CIPHER_CTX *cipher = NULL;
  uint8_t *buffer = NULL;
  int first_len = 0;
  int final_len = 0;
  int ok = -1;

  if (ciphertext_len > (size_t)INT_MAX) return -1;
  buffer = (uint8_t *)malloc(ciphertext_len + 1U);
  cipher = EVP_CIPHER_CTX_new();
  if (!buffer || !cipher) goto cleanup;
  if (EVP_DecryptInit_ex(cipher, EVP_aes_256_cbc(), NULL, key, iv) != 1 ||
      EVP_DecryptUpdate(cipher, buffer, &first_len, ciphertext,
                        (int)ciphertext_len) != 1 ||
      EVP_DecryptFinal_ex(cipher, buffer + first_len, &final_len) != 1) {
    goto cleanup;
  }
  buffer[first_len + final_len] = 0;
  *plaintext = buffer;
  *plaintext_len = (size_t)(first_len + final_len);
  buffer = NULL;
  ok = 0;

cleanup:
  EVP_CIPHER_CTX_free(cipher);
  free(buffer);
  return ok;
}

static int rsa_encrypt_key(X509 *cert, const uint8_t *key, size_t key_len,
                           uint8_t **encrypted, size_t *encrypted_len) {
  EVP_PKEY *public_key = NULL;
  EVP_PKEY_CTX *rsa = NULL;
  uint8_t *buffer = NULL;
  size_t required = 0;
  int ok = -1;

  public_key = X509_get_pubkey(cert);
  if (!public_key) goto cleanup;
  rsa = EVP_PKEY_CTX_new(public_key, NULL);
  if (!rsa || EVP_PKEY_encrypt_init(rsa) <= 0 ||
      EVP_PKEY_CTX_set_rsa_padding(rsa, RSA_PKCS1_PADDING) <= 0 ||
      EVP_PKEY_encrypt(rsa, NULL, &required, key, key_len) <= 0) {
    goto cleanup;
  }
  buffer = (uint8_t *)malloc(required);
  if (!buffer ||
      EVP_PKEY_encrypt(rsa, buffer, &required, key, key_len) <= 0) {
    goto cleanup;
  }
  *encrypted = buffer;
  *encrypted_len = required;
  buffer = NULL;
  ok = 0;

cleanup:
  free(buffer);
  EVP_PKEY_CTX_free(rsa);
  EVP_PKEY_free(public_key);
  return ok;
}

static int rsa_decrypt_key(EVP_PKEY *private_key, const uint8_t *encrypted,
                           size_t encrypted_len,
                           uint8_t key[MIME_SMIME_AES_KEY_SIZE]) {
  EVP_PKEY_CTX *rsa = EVP_PKEY_CTX_new(private_key, NULL);
  uint8_t *buffer = NULL;
  size_t required = 0;
  int ok = -1;
  if (!rsa || EVP_PKEY_decrypt_init(rsa) <= 0 ||
      EVP_PKEY_CTX_set_rsa_padding(rsa, RSA_PKCS1_PADDING) <= 0 ||
      EVP_PKEY_decrypt(rsa, NULL, &required, encrypted, encrypted_len) <= 0) {
    goto cleanup;
  }
  buffer = (uint8_t *)malloc(required);
  if (!buffer ||
      EVP_PKEY_decrypt(rsa, buffer, &required, encrypted, encrypted_len) <= 0 ||
      required != MIME_SMIME_AES_KEY_SIZE) {
    goto cleanup;
  }
  memcpy(key, buffer, required);
  ok = 0;

cleanup:
  if (buffer) OPENSSL_cleanse(buffer, required);
  free(buffer);
  EVP_PKEY_CTX_free(rsa);
  return ok;
}

static int rsa_sha256_sign(EVP_PKEY *private_key, const uint8_t *data,
                           size_t data_len, uint8_t **signature,
                           size_t *signature_len) {
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  uint8_t *buffer = NULL;
  size_t required = 0;
  int ok = -1;
  if (!digest || EVP_DigestSignInit(digest, NULL, EVP_sha256(), NULL,
                                    private_key) != 1 ||
      EVP_DigestSignUpdate(digest, data, data_len) != 1 ||
      EVP_DigestSignFinal(digest, NULL, &required) != 1) {
    goto cleanup;
  }
  buffer = (uint8_t *)malloc(required);
  if (!buffer || EVP_DigestSignFinal(digest, buffer, &required) != 1) {
    goto cleanup;
  }
  *signature = buffer;
  *signature_len = required;
  buffer = NULL;
  ok = 0;

cleanup:
  free(buffer);
  EVP_MD_CTX_free(digest);
  return ok;
}

static int rsa_sha256_verify(X509 *cert, const uint8_t *data, size_t data_len,
                             const uint8_t *signature, size_t signature_len) {
  EVP_PKEY *public_key = X509_get_pubkey(cert);
  EVP_MD_CTX *digest = EVP_MD_CTX_new();
  int ok = -1;
  if (public_key && digest &&
      EVP_DigestVerifyInit(digest, NULL, EVP_sha256(), NULL, public_key) == 1 &&
      EVP_DigestVerifyUpdate(digest, data, data_len) == 1 &&
      EVP_DigestVerifyFinal(digest, signature, signature_len) == 1) {
    ok = 0;
  }
  EVP_MD_CTX_free(digest);
  EVP_PKEY_free(public_key);
  return ok;
}

static int certificate_identifier(X509 *cert,
                                  uint8_t identifier[MIME_SMIME_SHA256_SIZE]) {
  uint8_t *der = NULL;
  int der_len = i2d_X509(cert, &der);
  unsigned int digest_len = 0U;
  int result;
  if (der_len <= 0 || !der) return -1;
  result = EVP_Digest(der, (size_t)der_len, identifier, &digest_len,
                      EVP_sha256(), NULL);
  OPENSSL_free(der);
  return result == 1 && digest_len == MIME_SMIME_SHA256_SIZE ? 0 : -1;
}

static asn1_value_t *build_enveloped_data(
    const uint8_t identifier[MIME_SMIME_SHA256_SIZE],
    const uint8_t *encrypted_key, size_t encrypted_key_len,
    const uint8_t iv[MIME_SMIME_AES_IV_SIZE], const uint8_t *ciphertext,
    size_t ciphertext_len) {
  asn1_value_t *content_info = NULL;
  asn1_value_t *explicit_content = NULL;
  asn1_value_t *enveloped = NULL;
  asn1_value_t *recipients = NULL;
  asn1_value_t *recipient = NULL;
  asn1_value_t *encrypted_content_info = NULL;

  content_info = asn1_create_sequence();
  explicit_content = context_value(0, 1, NULL, 0);
  enveloped = asn1_create_sequence();
  recipients = asn1_create_set();
  recipient = asn1_create_sequence();
  encrypted_content_info = asn1_create_sequence();
  if (!content_info || !explicit_content || !enveloped || !recipients ||
      !recipient || !encrypted_content_info) goto fail;

  if (add_sequence_child(content_info,
                         asn1_create_oid_from_string(OID_CMS_ENVELOPED_DATA)) ||
      add_sequence_child(enveloped, asn1_create_integer(2)) ||
      add_sequence_child(recipient, asn1_create_integer(2)) ||
      add_sequence_child(recipient,
                         context_value(0, 0, identifier,
                                       MIME_SMIME_SHA256_SIZE)) ||
      add_sequence_child(recipient,
                         algorithm_identifier(OID_RSA_ENCRYPTION, NULL, 0)) ||
      add_sequence_child(recipient,
                         asn1_create_octet_string(encrypted_key,
                                                  encrypted_key_len)) ||
      add_set_child(recipients, recipient) ||
      add_sequence_child(enveloped, recipients) ||
      add_sequence_child(encrypted_content_info,
                         asn1_create_oid_from_string(OID_CMS_DATA)) ||
      add_sequence_child(encrypted_content_info,
                         algorithm_identifier(OID_AES256_CBC, iv,
                                              MIME_SMIME_AES_IV_SIZE)) ||
      add_sequence_child(encrypted_content_info,
                         context_value(0, 0, ciphertext, ciphertext_len)) ||
      add_sequence_child(enveloped, encrypted_content_info) ||
      add_sequence_child(explicit_content, enveloped) ||
      add_sequence_child(content_info, explicit_content)) {
    goto fail_owned;
  }
  return content_info;

fail:
  asn1_free(recipient);
  asn1_free(recipients);
  asn1_free(encrypted_content_info);
  asn1_free(enveloped);
  asn1_free(explicit_content);
  asn1_free(content_info);
  return NULL;
fail_owned:
  asn1_free(content_info);
  return NULL;
}

static asn1_value_t *build_signed_data(
    const uint8_t identifier[MIME_SMIME_SHA256_SIZE],
    const uint8_t *signature, size_t signature_len) {
  asn1_value_t *content_info = asn1_create_sequence();
  asn1_value_t *explicit_content = context_value(0, 1, NULL, 0);
  asn1_value_t *signed_data = asn1_create_sequence();
  asn1_value_t *digest_algorithms = asn1_create_set();
  asn1_value_t *encapsulated = asn1_create_sequence();
  asn1_value_t *signer_infos = asn1_create_set();
  asn1_value_t *signer = asn1_create_sequence();
  if (!content_info || !explicit_content || !signed_data ||
      !digest_algorithms || !encapsulated || !signer_infos || !signer) {
    goto fail;
  }
  if (add_sequence_child(content_info,
                         asn1_create_oid_from_string(OID_CMS_SIGNED_DATA)) ||
      add_sequence_child(signed_data, asn1_create_integer(3)) ||
      add_set_child(digest_algorithms,
                    algorithm_identifier(OID_SHA256, NULL, 0)) ||
      add_sequence_child(signed_data, digest_algorithms) ||
      add_sequence_child(encapsulated,
                         asn1_create_oid_from_string(OID_CMS_DATA)) ||
      add_sequence_child(signed_data, encapsulated) ||
      add_sequence_child(signer, asn1_create_integer(3)) ||
      add_sequence_child(signer,
                         context_value(0, 0, identifier,
                                       MIME_SMIME_SHA256_SIZE)) ||
      add_sequence_child(signer,
                         algorithm_identifier(OID_SHA256, NULL, 0)) ||
      add_sequence_child(signer,
                         algorithm_identifier(OID_RSA_ENCRYPTION, NULL, 0)) ||
      add_sequence_child(signer,
                         asn1_create_octet_string(signature, signature_len)) ||
      add_set_child(signer_infos, signer) ||
      add_sequence_child(signed_data, signer_infos) ||
      add_sequence_child(explicit_content, signed_data) ||
      add_sequence_child(content_info, explicit_content)) {
    asn1_free(content_info);
    return NULL;
  }
  return content_info;

fail:
  asn1_free(signer);
  asn1_free(signer_infos);
  asn1_free(encapsulated);
  asn1_free(digest_algorithms);
  asn1_free(signed_data);
  asn1_free(explicit_content);
  asn1_free(content_info);
  return NULL;
}

static char *fold_base64(const char *base64, size_t *output_len) {
  size_t input_len = strlen(base64);
  size_t lines = (input_len + 63U) / 64U;
  char *folded = (char *)malloc(input_len + lines * 2U + 1U);
  size_t in_pos = 0;
  size_t out_pos = 0;
  if (!folded) return NULL;
  while (in_pos < input_len) {
    size_t chunk = input_len - in_pos;
    if (chunk > 64U) chunk = 64U;
    memcpy(folded + out_pos, base64 + in_pos, chunk);
    in_pos += chunk;
    out_pos += chunk;
    memcpy(folded + out_pos, "\r\n", 2);
    out_pos += 2U;
  }
  folded[out_pos] = '\0';
  *output_len = out_pos;
  return folded;
}

static char *wrap_enveloped_mime(const uint8_t *der, size_t der_len,
                                 size_t *output_len) {
  static const char header[] =
      "MIME-Version: 1.0\r\n"
      "Content-Type: application/pkcs7-mime; smime-type=enveloped-data; "
      "name=smime.p7m\r\n"
      "Content-Transfer-Encoding: base64\r\n\r\n";
  char *base64 = NULL;
  char *folded = NULL;
  char *result = NULL;
  size_t folded_len = 0;
  if (tn_base64_encode(der, der_len, &base64) != 0 || !base64) goto cleanup;
  folded = fold_base64(base64, &folded_len);
  if (!folded) goto cleanup;
  result = (char *)malloc(sizeof(header) - 1U + folded_len + 1U);
  if (!result) goto cleanup;
  memcpy(result, header, sizeof(header) - 1U);
  memcpy(result + sizeof(header) - 1U, folded, folded_len + 1U);
  *output_len = sizeof(header) - 1U + folded_len;

cleanup:
  free(base64);
  free(folded);
  return result;
}

static char *wrap_signed_mime(const char *message, size_t message_len,
                              const uint8_t *der, size_t der_len,
                              size_t *output_len) {
  static const char prefix[] =
      "MIME-Version: 1.0\r\n"
      "Content-Type: multipart/signed; "
      "protocol=application/pkcs7-signature; micalg=sha-256; boundary=\""
      MIME_SMIME_BOUNDARY "\"\r\n\r\n--" MIME_SMIME_BOUNDARY
      "\r\nContent-Type: application/octet-stream\r\n\r\n";
  static const char middle[] =
      "\r\n--" MIME_SMIME_BOUNDARY
      "\r\nContent-Type: application/pkcs7-signature; name=smime.p7s\r\n"
      "Content-Transfer-Encoding: base64\r\n\r\n";
  static const char suffix[] = "--" MIME_SMIME_BOUNDARY "--\r\n";
  char *base64 = NULL;
  char *folded = NULL;
  char *result = NULL;
  size_t folded_len = 0;
  size_t total;
  size_t pos = 0;
  if (tn_base64_encode(der, der_len, &base64) != 0 || !base64) goto cleanup;
  folded = fold_base64(base64, &folded_len);
  if (!folded) goto cleanup;
  total = sizeof(prefix) - 1U + message_len + sizeof(middle) - 1U +
          folded_len + sizeof(suffix) - 1U;
  result = (char *)malloc(total + 1U);
  if (!result) goto cleanup;
  memcpy(result + pos, prefix, sizeof(prefix) - 1U);
  pos += sizeof(prefix) - 1U;
  memcpy(result + pos, message, message_len);
  pos += message_len;
  memcpy(result + pos, middle, sizeof(middle) - 1U);
  pos += sizeof(middle) - 1U;
  memcpy(result + pos, folded, folded_len);
  pos += folded_len;
  memcpy(result + pos, suffix, sizeof(suffix) - 1U);
  pos += sizeof(suffix) - 1U;
  result[pos] = '\0';
  *output_len = pos;

cleanup:
  free(base64);
  free(folded);
  return result;
}

static const char *find_bytes(const char *data, size_t data_len,
                              const char *needle, size_t needle_len) {
  size_t i;
  if (needle_len == 0 || data_len < needle_len) return NULL;
  for (i = 0; i <= data_len - needle_len; ++i) {
    if (memcmp(data + i, needle, needle_len) == 0) return data + i;
  }
  return NULL;
}

static int decode_base64_region(const char *input, size_t input_len,
                                uint8_t **decoded, size_t *decoded_len) {
  char *normalized = (char *)malloc(input_len + 1U);
  size_t length = 0;
  size_t i;
  int result;
  if (!normalized) return -1;
  for (i = 0; i < input_len; ++i) {
    if (!isspace((unsigned char)input[i])) normalized[length++] = input[i];
  }
  normalized[length] = '\0';
  result = tn_base64_decode(normalized, decoded, decoded_len);
  free(normalized);
  return result == 0 ? 0 : -1;
}

static int decode_mime_body(const char *input, size_t input_len,
                            uint8_t **decoded, size_t *decoded_len) {
  const char *body = find_bytes(input, input_len, "\r\n\r\n", 4);
  size_t body_len;
  if (body) {
    body += 4;
  } else {
    body = input;
  }
  body_len = input_len - (size_t)(body - input);
  return decode_base64_region(body, body_len, decoded, decoded_len);
}

static int parse_signed_mime(const char *input, size_t input_len,
                             const uint8_t **content, size_t *content_len,
                             uint8_t **der, size_t *der_len) {
  static const char part_start[] =
      "--" MIME_SMIME_BOUNDARY
      "\r\nContent-Type: application/octet-stream\r\n\r\n";
  static const char signature_start[] =
      "\r\n--" MIME_SMIME_BOUNDARY
      "\r\nContent-Type: application/pkcs7-signature; name=smime.p7s\r\n"
      "Content-Transfer-Encoding: base64\r\n\r\n";
  static const char signature_end[] = "--" MIME_SMIME_BOUNDARY "--";
  const char *begin = find_bytes(input, input_len, part_start,
                                 sizeof(part_start) - 1U);
  const char *middle;
  const char *end;
  size_t remaining;
  if (!begin) return -1;
  begin += sizeof(part_start) - 1U;
  remaining = input_len - (size_t)(begin - input);
  middle = find_bytes(begin, remaining, signature_start,
                      sizeof(signature_start) - 1U);
  if (!middle) return -1;
  *content = (const uint8_t *)begin;
  *content_len = (size_t)(middle - begin);
  middle += sizeof(signature_start) - 1U;
  remaining = input_len - (size_t)(middle - input);
  end = find_bytes(middle, remaining, signature_end,
                   sizeof(signature_end) - 1U);
  if (!end) return -1;
  return decode_base64_region(middle, (size_t)(end - middle), der, der_len);
}

mime_smime_ctx_t *mime_smime_ctx_create(void) {
  return (mime_smime_ctx_t *)calloc(1, sizeof(mime_smime_ctx_t));
}

void mime_smime_ctx_free(mime_smime_ctx_t *ctx) {
  if (!ctx) return;
  X509_free(ctx->cert);
  EVP_PKEY_free(ctx->pkey);
  free(ctx);
}

int mime_smime_load_cert(mime_smime_ctx_t *ctx, const char *cert_path) {
  FILE *file;
  X509 *cert;
  if (!ctx || !cert_path) return MIME_SMIME_ERROR_CERT_LOAD;
  file = fopen(cert_path, "rb");
  if (!file) {
    set_error(ctx, "Failed to open certificate file");
    return MIME_SMIME_ERROR_CERT_LOAD;
  }
  cert = PEM_read_X509(file, NULL, NULL, NULL);
  fclose(file);
  if (!cert) {
    set_error(ctx, "Failed to parse certificate");
    return MIME_SMIME_ERROR_CERT_LOAD;
  }
  X509_free(ctx->cert);
  ctx->cert = cert;
  return MIME_SMIME_OK;
}

int mime_smime_load_cert_mem(mime_smime_ctx_t *ctx, const char *cert_pem,
                             size_t len) {
  BIO *bio;
  X509 *cert;
  if (!ctx || !cert_pem || len > (size_t)INT_MAX)
    return MIME_SMIME_ERROR_CERT_LOAD;
  bio = BIO_new_mem_buf(cert_pem, (int)len);
  if (!bio) return MIME_SMIME_ERROR_MEMORY;
  cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
  BIO_free(bio);
  if (!cert) {
    set_error(ctx, "Failed to parse certificate from memory");
    return MIME_SMIME_ERROR_CERT_LOAD;
  }
  X509_free(ctx->cert);
  ctx->cert = cert;
  return MIME_SMIME_OK;
}

int mime_smime_load_key(mime_smime_ctx_t *ctx, const char *key_path,
                        const char *password) {
  FILE *file;
  EVP_PKEY *key;
  if (!ctx || !key_path) return MIME_SMIME_ERROR_KEY_LOAD;
  file = fopen(key_path, "rb");
  if (!file) {
    set_error(ctx, "Failed to open private key file");
    return MIME_SMIME_ERROR_KEY_LOAD;
  }
  key = PEM_read_PrivateKey(file, NULL, NULL, (void *)password);
  fclose(file);
  if (!key) {
    set_error(ctx, "Failed to parse private key");
    return MIME_SMIME_ERROR_KEY_LOAD;
  }
  EVP_PKEY_free(ctx->pkey);
  ctx->pkey = key;
  return MIME_SMIME_OK;
}

int mime_smime_load_key_mem(mime_smime_ctx_t *ctx, const char *key_pem,
                            size_t len, const char *password) {
  BIO *bio;
  EVP_PKEY *key;
  if (!ctx || !key_pem || len > (size_t)INT_MAX)
    return MIME_SMIME_ERROR_KEY_LOAD;
  bio = BIO_new_mem_buf(key_pem, (int)len);
  if (!bio) return MIME_SMIME_ERROR_MEMORY;
  key = PEM_read_bio_PrivateKey(bio, NULL, NULL, (void *)password);
  BIO_free(bio);
  if (!key) {
    set_error(ctx, "Failed to parse private key from memory");
    return MIME_SMIME_ERROR_KEY_LOAD;
  }
  EVP_PKEY_free(ctx->pkey);
  ctx->pkey = key;
  return MIME_SMIME_OK;
}

char *mime_smime_encrypt(mime_smime_ctx_t *ctx, const char *message,
                         size_t message_len, size_t *output_len,
                         mime_smime_error_t *error) {
  uint8_t key[MIME_SMIME_AES_KEY_SIZE];
  uint8_t iv[MIME_SMIME_AES_IV_SIZE];
  uint8_t identifier[MIME_SMIME_SHA256_SIZE];
  uint8_t *ciphertext = NULL;
  size_t ciphertext_len = 0;
  uint8_t *encrypted_key = NULL;
  size_t encrypted_key_len = 0;
  asn1_value_t *cms = NULL;
  uint8_t *der = NULL;
  size_t der_len = 0;
  char *result = NULL;
  mime_smime_error_t result_error = MIME_SMIME_ERROR_ENCRYPT;

  if (!ctx || !ctx->cert || (!message && message_len != 0) || !output_len)
    goto cleanup;
  if (RAND_bytes(key, (int)sizeof(key)) != 1 ||
      RAND_bytes(iv, (int)sizeof(iv)) != 1 ||
      certificate_identifier(ctx->cert, identifier) != 0 ||
      encrypt_aes256_cbc((const uint8_t *)message, message_len, key, iv,
                         &ciphertext, &ciphertext_len) != 0 ||
      rsa_encrypt_key(ctx->cert, key, sizeof(key), &encrypted_key,
                      &encrypted_key_len) != 0) {
    set_error(ctx, "S/MIME encryption failed");
    goto cleanup;
  }
  cms = build_enveloped_data(identifier, encrypted_key, encrypted_key_len, iv,
                             ciphertext, ciphertext_len);
  if (!cms || encode_der(cms, &der, &der_len) != 0 ||
      !(result = wrap_enveloped_mime(der, der_len, output_len))) {
    result_error = MIME_SMIME_ERROR_MEMORY;
    set_error(ctx, "Failed to encode S/MIME enveloped data");
    goto cleanup;
  }
  result_error = MIME_SMIME_OK;

cleanup:
  OPENSSL_cleanse(key, sizeof(key));
  free(ciphertext);
  free(encrypted_key);
  asn1_free(cms);
  free(der);
  if (error) *error = result_error;
  return result;
}

char *mime_smime_decrypt(mime_smime_ctx_t *ctx, const char *encrypted,
                         size_t encrypted_len, size_t *output_len,
                         mime_smime_error_t *error) {
  uint8_t *der = NULL;
  size_t der_len = 0;
  asn1_value_t *root = NULL;
  asn1_value_t *enveloped;
  asn1_value_t *recipient;
  asn1_value_t *encrypted_content_info;
  asn1_value_t *algorithm;
  asn1_value_t *encrypted_key;
  asn1_value_t *ciphertext;
  asn1_value_t *iv;
  uint8_t key[MIME_SMIME_AES_KEY_SIZE];
  uint8_t *plaintext = NULL;
  size_t plaintext_len = 0;
  mime_smime_error_t result_error = MIME_SMIME_ERROR_DECRYPT;

  if (!ctx || !ctx->pkey || !encrypted || !output_len ||
      decode_mime_body(encrypted, encrypted_len, &der, &der_len) != 0 ||
      asn1_der_decode(der, der_len, &root) != 0 || !root ||
      root->type != ASN1_TYPE_SEQUENCE || root->value.sequence.count != 2 ||
      !oid_equals(root->value.sequence.children[0], OID_CMS_ENVELOPED_DATA) ||
      root->value.sequence.children[1]->type != TK_CONTEXT_SPECIFIC ||
      !root->value.sequence.children[1]->constructed ||
      root->value.sequence.children[1]->value.sequence.count != 1) {
    set_error(ctx, "Invalid S/MIME enveloped data");
    goto cleanup;
  }
  enveloped = root->value.sequence.children[1]->value.sequence.children[0];
  if (enveloped->type != ASN1_TYPE_SEQUENCE ||
      enveloped->value.sequence.count != 3 ||
      enveloped->value.sequence.children[1]->type != ASN1_TYPE_SET ||
      enveloped->value.sequence.children[1]->value.set.count != 1) {
    set_error(ctx, "Unsupported S/MIME recipient structure");
    goto cleanup;
  }
  recipient = enveloped->value.sequence.children[1]->value.set.children[0];
  encrypted_content_info = enveloped->value.sequence.children[2];
  if (recipient->type != ASN1_TYPE_SEQUENCE ||
      recipient->value.sequence.count != 4 ||
      encrypted_content_info->type != ASN1_TYPE_SEQUENCE ||
      encrypted_content_info->value.sequence.count != 3) {
    set_error(ctx, "Invalid S/MIME encrypted content");
    goto cleanup;
  }
  encrypted_key = recipient->value.sequence.children[3];
  algorithm = encrypted_content_info->value.sequence.children[1];
  ciphertext = encrypted_content_info->value.sequence.children[2];
  if (encrypted_key->type != ASN1_TYPE_OCTET_STRING ||
      algorithm->type != ASN1_TYPE_SEQUENCE ||
      algorithm->value.sequence.count != 2 ||
      !oid_equals(algorithm->value.sequence.children[0], OID_AES256_CBC) ||
      (iv = algorithm->value.sequence.children[1])->type !=
          ASN1_TYPE_OCTET_STRING ||
      iv->value.octet_string.length != MIME_SMIME_AES_IV_SIZE ||
      ciphertext->type != TK_CONTEXT_SPECIFIC || ciphertext->constructed) {
    set_error(ctx, "Unsupported S/MIME encryption algorithm");
    goto cleanup;
  }
  if (rsa_decrypt_key(ctx->pkey, encrypted_key->value.octet_string.data,
                      encrypted_key->value.octet_string.length, key) != 0 ||
      decrypt_aes256_cbc(ciphertext->value.octet_string.data,
                         ciphertext->value.octet_string.length, key,
                         iv->value.octet_string.data, &plaintext,
                         &plaintext_len) != 0) {
    set_error(ctx, "S/MIME decryption failed");
    goto cleanup;
  }
  *output_len = plaintext_len;
  result_error = MIME_SMIME_OK;

cleanup:
  OPENSSL_cleanse(key, sizeof(key));
  free(der);
  asn1_free(root);
  if (error) *error = result_error;
  return (char *)plaintext;
}

char *mime_smime_sign(mime_smime_ctx_t *ctx, const char *message,
                      size_t message_len, size_t *output_len,
                      mime_smime_error_t *error) {
  uint8_t identifier[MIME_SMIME_SHA256_SIZE];
  uint8_t *signature = NULL;
  size_t signature_len = 0;
  asn1_value_t *cms = NULL;
  uint8_t *der = NULL;
  size_t der_len = 0;
  char *result = NULL;
  mime_smime_error_t result_error = MIME_SMIME_ERROR_SIGN;
  if (!ctx || !ctx->cert || !ctx->pkey || (!message && message_len != 0) ||
      !output_len) {
    goto cleanup;
  }
  if (X509_check_private_key(ctx->cert, ctx->pkey) != 1 ||
      certificate_identifier(ctx->cert, identifier) != 0 ||
      rsa_sha256_sign(ctx->pkey, (const uint8_t *)message, message_len,
                      &signature, &signature_len) != 0) {
    set_error(ctx, "S/MIME signing failed");
    goto cleanup;
  }
  cms = build_signed_data(identifier, signature, signature_len);
  if (!cms || encode_der(cms, &der, &der_len) != 0 ||
      !(result = wrap_signed_mime(message, message_len, der, der_len,
                                  output_len))) {
    result_error = MIME_SMIME_ERROR_MEMORY;
    set_error(ctx, "Failed to encode S/MIME signed data");
    goto cleanup;
  }
  result_error = MIME_SMIME_OK;

cleanup:
  free(signature);
  asn1_free(cms);
  free(der);
  if (error) *error = result_error;
  return result;
}

int mime_smime_verify(mime_smime_ctx_t *ctx, const char *signed_data,
                      size_t signed_len, char **output, size_t *output_len) {
  const uint8_t *content = NULL;
  size_t content_len = 0;
  uint8_t *der = NULL;
  size_t der_len = 0;
  asn1_value_t *root = NULL;
  asn1_value_t *signed_content;
  asn1_value_t *signer_infos;
  asn1_value_t *signer;
  asn1_value_t *signature;
  int result = MIME_SMIME_ERROR_VERIFY;

  if (!ctx || !ctx->cert || !signed_data ||
      parse_signed_mime(signed_data, signed_len, &content, &content_len, &der,
                        &der_len) != 0 ||
      asn1_der_decode(der, der_len, &root) != 0 || !root ||
      root->type != ASN1_TYPE_SEQUENCE || root->value.sequence.count != 2 ||
      !oid_equals(root->value.sequence.children[0], OID_CMS_SIGNED_DATA) ||
      root->value.sequence.children[1]->type != TK_CONTEXT_SPECIFIC ||
      !root->value.sequence.children[1]->constructed ||
      root->value.sequence.children[1]->value.sequence.count != 1) {
    set_error(ctx, "Invalid S/MIME signed data");
    goto cleanup;
  }
  signed_content =
      root->value.sequence.children[1]->value.sequence.children[0];
  if (signed_content->type != ASN1_TYPE_SEQUENCE ||
      signed_content->value.sequence.count != 4 ||
      (signer_infos = signed_content->value.sequence.children[3])->type !=
          ASN1_TYPE_SET ||
      signer_infos->value.set.count != 1 ||
      (signer = signer_infos->value.set.children[0])->type !=
          ASN1_TYPE_SEQUENCE ||
      signer->value.sequence.count != 5 ||
      (signature = signer->value.sequence.children[4])->type !=
          ASN1_TYPE_OCTET_STRING) {
    set_error(ctx, "Unsupported S/MIME signer structure");
    goto cleanup;
  }
  if (rsa_sha256_verify(ctx->cert, content, content_len,
                        signature->value.octet_string.data,
                        signature->value.octet_string.length) != 0) {
    set_error(ctx, "S/MIME signature verification failed");
    goto cleanup;
  }
  if (output) {
    *output = (char *)malloc(content_len + 1U);
    if (!*output) {
      result = MIME_SMIME_ERROR_MEMORY;
      goto cleanup;
    }
    memcpy(*output, content, content_len);
    (*output)[content_len] = '\0';
  }
  if (output_len) *output_len = content_len;
  result = MIME_SMIME_OK;

cleanup:
  free(der);
  asn1_free(root);
  return result;
}

int mime_is_smime_encrypted(const char *content_type, size_t len) {
  if (!content_type || len < 20) return 0;
  return cmeta_scan_mem(content_type, len, "application/pkcs7-mime", 22) !=
             NULL &&
         cmeta_scan_mem(content_type, len, "smime-type=enveloped-data", 25) !=
             NULL;
}

int mime_is_smime_signed(const char *content_type, size_t len) {
  if (!content_type || len < 20) return 0;
  return (cmeta_scan_mem(content_type, len, "application/pkcs7-mime", 22) !=
              NULL &&
          cmeta_scan_mem(content_type, len, "smime-type=signed-data", 22) !=
              NULL) ||
         (cmeta_scan_mem(content_type, len, "multipart/signed", 16) != NULL &&
          cmeta_scan_mem(content_type, len,
                         "protocol=application/pkcs7-signature", 36) != NULL);
}

const char *mime_smime_get_error(mime_smime_ctx_t *ctx) {
  return ctx ? ctx->error_msg : "Invalid context";
}
