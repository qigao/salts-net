// SPDX-FileCopyrightText: 2017-2022 Comcast Cable Communications Management, LLC
// SPDX-License-Identifier: Apache-2.0

#include <cjson/cJSON.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cjwt.h"
#include "jws.h"
#include "jwe.h"
#include "utils.h"

/*----------------------------------------------------------------------------*/
/*                                   Macros                                   */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/
struct alg_map {
    cjwt_alg_t alg;
    bool symmetric;
    const char *text;
};

/*----------------------------------------------------------------------------*/
/*                            File Scoped Variables                           */
/*----------------------------------------------------------------------------*/
const struct alg_map the_alg_map[] = {
    {.alg = alg_none,  .symmetric = false, .text = "none" },
    {.alg = alg_es256, .symmetric = false, .text = "ES256"},
    {.alg = alg_es384, .symmetric = false, .text = "ES384"},
    {.alg = alg_es512, .symmetric = false, .text = "ES512"},
    {.alg = alg_hs256, .symmetric = true,  .text = "HS256"},
    {.alg = alg_hs384, .symmetric = true,  .text = "HS384"},
    {.alg = alg_hs512, .symmetric = true,  .text = "HS512"},
    {.alg = alg_ps256, .symmetric = false, .text = "PS256"},
    {.alg = alg_ps384, .symmetric = false, .text = "PS384"},
    {.alg = alg_ps512, .symmetric = false, .text = "PS512"},
    {.alg = alg_rs256, .symmetric = false, .text = "RS256"},
    {.alg = alg_rs384, .symmetric = false, .text = "RS384"},
    {.alg = alg_rs512, .symmetric = false, .text = "RS512"},
    {.alg = alg_es256k, .symmetric = false, .text = "ES256K"},
    {.alg = alg_eddsa, .symmetric = false, .text = "EdDSA"},
    {.alg = alg_rsa_oaep, .symmetric = false, .text = "RSA-OAEP"},
    {.alg = alg_rsa_oaep_256, .symmetric = false, .text = "RSA-OAEP-256"},
    {.alg = alg_dir, .symmetric = false, .text = "dir"},
    {.alg = alg_a128kw, .symmetric = false, .text = "A128KW"},
    {.alg = alg_a192kw, .symmetric = false, .text = "A192KW"},
    {.alg = alg_a256kw, .symmetric = false, .text = "A256KW"},
    {.alg = alg_pbes2_hs256_a128kw, .symmetric = false, .text = "PBES2-HS256+A128KW"},
    {.alg = alg_pbes2_hs384_a192kw, .symmetric = false, .text = "PBES2-HS384+A192KW"},
    {.alg = alg_pbes2_hs512_a256kw, .symmetric = false, .text = "PBES2-HS512+A256KW"}
};

struct enc_map {
    cjwt_enc_t enc;
    const char *text;
};

const struct enc_map the_enc_map[] = {
    {.enc = enc_a128gcm, .text = "A128GCM"},
    {.enc = enc_a192gcm, .text = "A192GCM"},
    {.enc = enc_a256gcm, .text = "A256GCM"},
    {.enc = enc_a128cbc_hs256, .text = "A128CBC-HS256"},
    {.enc = enc_a192cbc_hs384, .text = "A192CBC-HS384"},
    {.enc = enc_a256cbc_hs512, .text = "A256CBC-HS512"}
};

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/
/* none */

/*----------------------------------------------------------------------------*/
/*                             Internal functions                             */
/*----------------------------------------------------------------------------*/

static int alg_to_enum(const char *alg_str, cjwt_alg_t *alg)
{
    for (size_t i = 0; i < sizeof(the_alg_map) / sizeof(struct alg_map); i++) {
        if (!strcmp(alg_str, the_alg_map[i].text)) {
            *alg = the_alg_map[i].alg;
            return 0;
        }
    }

    return -1;
}

const char *alg_to_string(cjwt_alg_t alg)
{
    for (size_t i = 0; i < sizeof(the_alg_map) / sizeof(struct alg_map); i++) {
        if (alg == the_alg_map[i].alg) {
            return the_alg_map[i].text;
        }
    }

    return "unknown";
}

static int enc_to_enum(const char *enc_str, cjwt_enc_t *enc)
{
    for (size_t i = 0; i < sizeof(the_enc_map) / sizeof(struct enc_map); i++) {
        if (!strcmp(enc_str, the_enc_map[i].text)) {
            *enc = the_enc_map[i].enc;
            return 0;
        }
    }

    return -1;
}

const char *enc_to_string(cjwt_enc_t enc)
{
    for (size_t i = 0; i < sizeof(the_enc_map) / sizeof(struct enc_map); i++) {
        if (enc == the_enc_map[i].enc) {
            return the_enc_map[i].text;
        }
    }

    return "unknown";
}

static void delete_public_claims(cJSON *json)
{
    const char *claims[] = { "iss",
                             "sub",
                             "aud",
                             "jti",
                             "exp",
                             "nbf",
                             "iat" };

    for (size_t i = 0; i < sizeof(claims) / sizeof(char *); i++) {
        cJSON_DeleteItemFromObjectCaseSensitive(json, claims[i]);
    }
}


static cjwt_code_t process_string(const cJSON *json, const char *name, char **dest)
{
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(json, name);

    if (val) {
        if (val->type != cJSON_String) {
            return CJWTE_PAYLOAD_EXPECTED_STRING;
        }

        *dest = cjwt_strdup(val->valuestring);
        if (!(*dest)) {
            return CJWTE_OUT_OF_MEMORY;
        }
    }

    return CJWTE_OK;
}

static cjwt_code_t process_time(const cJSON *json, const char *name, int64_t **dest)
{
    const cJSON *val = cJSON_GetObjectItemCaseSensitive(json, name);

    if (val) {
        if (val->type == cJSON_Number) {
            *dest = malloc(sizeof(int64_t));
            if (!(*dest)) {
                return CJWTE_OUT_OF_MEMORY;
            }

            **dest = val->valueint;
        } else {
            return CJWTE_PAYLOAD_EXPECTED_NUMBER;
        }
    }

    return CJWTE_OK;
}

static cjwt_code_t process_aud(const cJSON *json, cjwt_t *cjwt)
{
    const cJSON *tmp = NULL;
    const cJSON *aud = NULL;

    aud = cJSON_GetObjectItemCaseSensitive(json, "aud");

    if (!aud) {
        return CJWTE_OK;
    }

    if (cJSON_Array == aud->type) {
        cjwt->aud.count = cJSON_GetArraySize(aud);
        cjwt->aud.names = calloc(cjwt->aud.count, sizeof(char *));

        if (!cjwt->aud.names) {
            return CJWTE_OUT_OF_MEMORY;
        }

        for (int i = 0; i < cjwt->aud.count; i++) {
            tmp = cJSON_GetArrayItem(aud, i);

            if (tmp->type != cJSON_String) {
                return CJWTE_PAYLOAD_EXPECTED_STRING;
            }

            cjwt->aud.names[i] = cjwt_strdup(tmp->valuestring);
            if (!cjwt->aud.names[i]) {
                return CJWTE_OUT_OF_MEMORY;
            }
        }
    } else if (cJSON_String == aud->type) {
        cjwt->aud.count = 1;
        cjwt->aud.names = calloc(cjwt->aud.count, sizeof(char *));

        if (!cjwt->aud.names) {
            return CJWTE_OUT_OF_MEMORY;
        }

        cjwt->aud.names[0] = cjwt_strdup(aud->valuestring);
        if (!cjwt->aud.names[0]) {
            return CJWTE_OUT_OF_MEMORY;
        }
    } else {
        return CJWTE_PAYLOAD_EXPECTED_STRING;
    }

    return CJWTE_OK;
}

static cjwt_code_t process_payload_from_json(cjwt_t *cjwt, cJSON *json)
{
    cjwt_code_t rv = CJWTE_OK;
    rv |= process_string(json, "iss", &cjwt->iss);
    rv |= process_string(json, "sub", &cjwt->sub);
    rv |= process_string(json, "jti", &cjwt->jti);

    rv |= process_time(json, "exp", &cjwt->exp);
    rv |= process_time(json, "nbf", &cjwt->nbf);
    rv |= process_time(json, "iat", &cjwt->iat);

    rv |= process_aud(json, cjwt);

    /* The private_claims either is assigned the json blob or deletes it. */
    delete_public_claims(json);

    if (cJSON_GetArraySize(json)) {
        cjwt->private_claims = json;
    } else {
        cJSON_Delete(json);
    }
    return rv;
}

static cjwt_code_t process_payload(cjwt_t *cjwt, const char *payload, size_t len)
{
    cjwt_code_t rv     = CJWTE_OK;
    size_t decoded_len = 0;
    char *decoded      = NULL;
    cJSON *json        = NULL;

    decoded = (char *) b64url_decode_with_alloc((const uint8_t *) payload, len,
                                                 &decoded_len);
    if (!decoded) {
        return CJWTE_PAYLOAD_INVALID_BASE64;
    }

    json = cJSON_ParseWithLength(decoded, decoded_len);
    if (!json) {
        free(decoded);
        return CJWTE_PAYLOAD_INVALID_JSON;
    }

    rv = process_payload_from_json(cjwt, json);

    free(decoded);
    return rv;
}


static cjwt_code_t process_header_json(cjwt_t *cjwt, uint32_t options,
                                       cJSON *json)
{
    cjwt_code_t rv   = CJWTE_OK;
    const cJSON *alg = NULL;
    const cJSON *typ = NULL;

    alg = cJSON_GetObjectItemCaseSensitive(json, "alg");
    if (!alg) {
        return CJWTE_HEADER_MISSING_ALG;
    }

    if (alg->type != cJSON_String) {
        return CJWTE_HEADER_UNSUPPORTED_ALG;
    }

    if (0 != alg_to_enum(alg->valuestring, &cjwt->header.alg)) {
        return CJWTE_HEADER_UNSUPPORTED_ALG;
    }

    if (alg_none == cjwt->header.alg) {
        if (0 == (OPT_ALLOW_ALG_NONE & options)) {
            return CJWTE_HEADER_UNSUPPORTED_ALG;
        }
    }

    if (true == the_alg_map[cjwt->header.alg].symmetric) {
        if (!(OPT_ALLOW_ONLY_HS_ALG & options)) {
            return CJWTE_HEADER_UNSUPPORTED_ALG;
        }
    } else {
        if (OPT_ALLOW_ONLY_HS_ALG & options) {
            return CJWTE_HEADER_UNSUPPORTED_ALG;
        }
    }

    typ = cJSON_GetObjectItemCaseSensitive(json, "typ");
    if (typ && (0 == (OPT_ALLOW_ANY_TYP & options))) {
        const char *s = typ->valuestring;

        if (typ->type != cJSON_String) {
            return CJWTE_HEADER_UNSUPPORTED_TYP;
        }

        if ((('J' != s[0]) && ('j' != s[0]))
            || (('W' != s[1]) && ('w' != s[1]))
            || (('T' != s[2]) && ('t' != s[2]))
            || ('\0' != s[3]))
        {
            return CJWTE_HEADER_UNSUPPORTED_TYP;
        }
    }

    rv = process_string(json, "kid", &cjwt->header.kid);
    if (CJWTE_OK != rv) {
        return rv;
    }

    const cJSON *enc = cJSON_GetObjectItemCaseSensitive(json, "enc");
    if (enc) {
        if (enc->type != cJSON_String || 0 != enc_to_enum(enc->valuestring, &cjwt->header.enc)) {
            return CJWTE_HEADER_UNSUPPORTED_ALG;
        }
    } else if (cjwt->header.alg >= alg_rsa_oaep) {
        /* enc is required for JWE */
        return CJWTE_HEADER_UNSUPPORTED_ALG;
    }

    /* These headers are important for the JWT to be processed.  If not supported
     * the library should return an error to prevent a false success. */
    if ((NULL != cJSON_GetObjectItemCaseSensitive(json, "jku"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "jwk"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "x5u"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "x5c"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "x5t"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "x5ts256"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "cty"))
        || (NULL != cJSON_GetObjectItemCaseSensitive(json, "crit")))
    {
        return CJWTE_HEADER_UNSUPPORTED_UNKNOWN;
    }

    /* Remove everything supported & handled. */
    cJSON_DeleteItemFromObjectCaseSensitive(json, "alg");
    cJSON_DeleteItemFromObjectCaseSensitive(json, "enc");
    cJSON_DeleteItemFromObjectCaseSensitive(json, "typ");
    cJSON_DeleteItemFromObjectCaseSensitive(json, "kid");

    /* Everything left can be considered private headers. */
    if (cJSON_GetArraySize(json)) {
        cjwt->header.private_headers = json;
    } else {
        cJSON_Delete(json);
    }

    return CJWTE_OK;
}


static cjwt_code_t process_header(cjwt_t *cjwt, uint32_t options,
                                  const char *header, size_t len)
{
    cjwt_code_t rv;
    size_t decoded_len = 0;
    char *decoded      = NULL;
    cJSON *json        = NULL;

    decoded = (char *) b64url_decode_with_alloc((const uint8_t *) header, len,
                                                &decoded_len);
    if (!decoded) {
        return CJWTE_HEADER_INVALID_BASE64;
    }

    json = cJSON_ParseWithLength(decoded, decoded_len);
    if (!json) {
        free(decoded);
        return CJWTE_HEADER_INVALID_JSON;
    }

    rv = process_header_json(cjwt, options, json);

    if (CJWTE_OK != rv) {
        cJSON_Delete(json);
    }
    free(decoded);

    return rv;
}

static cjwt_code_t verify_signature(const cjwt_t *jwt,
                                    const uint8_t *full, size_t full_len,
                                    const char *enc_sig, size_t enc_sig_len,
                                    const uint8_t *key, size_t key_len,
                                    const cjwt_jwk_t *jwk)
{
    cjwt_code_t rv = CJWTE_OK;
    struct sig_input in = {0};
    uint8_t *sig;
    size_t sig_len;

    sig = b64url_decode_with_alloc((const uint8_t *) enc_sig,
                                   enc_sig_len, &sig_len);
    if (!sig) {
        return CJWTE_SIGNATURE_INVALID_BASE64;
    }

    in.full.data = full;
    in.full.len  = full_len;
    in.key.data  = key;
    in.key.len   = key_len;
    in.sig.len   = sig_len;
    in.sig.data  = sig;

    if (jwk) {
        rv = jws_jwk_to_pkey(jwk, &in.pkey);
        if (rv != CJWTE_OK) { free(sig); return rv; }
    }

    rv = jws_verify_signature(jwt, &in);

    if (in.pkey) jws_pkey_free(in.pkey);
    free(sig);
    return rv;
}


static cjwt_code_t verify_time_windows(const cjwt_t *jwt, uint32_t options,
                                       int64_t time, int64_t skew)
{
    if (OPT_ALLOW_ANY_TIME == (OPT_ALLOW_ANY_TIME & options)) {
        return CJWTE_OK;
    }

    if (jwt->nbf && ((time + skew) < *(jwt->nbf))) {
        return CJWTE_TIME_BEFORE_NBF;
    }

    if (jwt->exp && (*(jwt->exp) < (time - skew))) {
        return CJWTE_TIME_AFTER_EXP;
    }

    return CJWTE_OK;
}


/*----------------------------------------------------------------------------*/
/*                             External Functions                             */
/*----------------------------------------------------------------------------*/

static cjwt_code_t cjwt_decode_internal(const char *encoded, size_t enc_len, uint32_t options,
                                        const uint8_t *key, size_t key_len,
                                        const cjwt_jwk_t *jwk,
                                        int64_t time, int64_t skew, cjwt_t **jwt)
{
    cjwt_code_t rv = CJWTE_OK;
    struct split_jwt sections;
    const struct section *header  = NULL;
    const struct section *payload = NULL;
    cjwt_t *out                   = NULL;

    if (!encoded || !jwt || !enc_len) {
        return CJWTE_INVALID_PARAMETERS;
    }

    int section_count = split(encoded, enc_len, &sections);
    if (section_count <= 0) {
        return CJWTE_HEADER_MISSING;
    }

    /* JWS has 3 sections, JWE has 5. */
    if (3 == section_count) {
        header  = &sections.sections[0];
        payload = &sections.sections[1];

        if (!header->len) {
            return CJWTE_HEADER_MISSING;
        }

        if (!payload->len) {
            return CJWTE_PAYLOAD_MISSING;
        }

        out = calloc(1, sizeof(cjwt_t));
        if (!out) {
            return CJWTE_OUT_OF_MEMORY;
        }

        rv = process_header(out, options, header->data, header->len);
        if (rv) {
            goto invalid;
        }

        if (out->header.alg != alg_none) {
            const struct section *sig = &sections.sections[2];
            size_t signed_len         = 0;

            if (0 == sig->len) {
                rv = CJWTE_SIGNATURE_MISSING;
                goto invalid;
            }
            signed_len = header->len + payload->len + 1;

            rv = verify_signature(out, (const uint8_t *) encoded, signed_len,
                                  sig->data, sig->len, key, key_len, jwk);
            if (rv) {
                goto invalid;
            }
        }

        rv = process_payload(out, payload->data, payload->len);
    } else if (5 == section_count) {
        uint8_t *plaintext = NULL;
        size_t plaintext_len = 0;

        header = &sections.sections[0];
        if (!header->len) return CJWTE_HEADER_MISSING;

        out = calloc(1, sizeof(cjwt_t));
        if (!out) return CJWTE_OUT_OF_MEMORY;

        rv = process_header(out, options, header->data, header->len);
        if (rv) goto invalid;

        if (out->header.enc == enc_unknown) {
            rv = CJWTE_INVALID_SECTIONS;
            goto invalid;
        }

        rv = jwe_decrypt(out, &sections.sections[0], &sections.sections[1], 
                         &sections.sections[2], &sections.sections[3], &sections.sections[4],
                         key, key_len, jwk, &plaintext, &plaintext_len);
        if (rv != CJWTE_OK) goto invalid;

        /* plaintext can be a JWS or just claims. */
        cJSON *json = cJSON_ParseWithLength((const char *)plaintext, plaintext_len);
        if (!json) {
            rv = CJWTE_PAYLOAD_INVALID_JSON;
            free(plaintext);
            goto invalid;
        }
        
        rv = process_payload_from_json(out, json);
        free(plaintext);
    } else {
        return CJWTE_INVALID_SECTIONS;
    }
    if (rv) {
        goto invalid;
    }

    rv = verify_time_windows(out, options, time, skew);

invalid:

    if (rv) {
        cjwt_destroy(out);
    } else {
        *jwt = out;
    }

    return rv;
}

cjwt_code_t cjwt_decode(const char *encoded, size_t enc_len, uint32_t options,
                        const uint8_t *key, size_t key_len,
                        int64_t time, int64_t skew, cjwt_t **jwt)
{
    return cjwt_decode_internal(encoded, enc_len, options, key, key_len, NULL, time, skew, jwt);
}

cjwt_code_t cjwt_decode_with_jwk(const char *encoded, size_t enc_len, uint32_t options,
                                 const cjwt_jwk_t *jwk,
                                 int64_t time, int64_t skew, cjwt_t **jwt)
{
    return cjwt_decode_internal(encoded, enc_len, options, NULL, 0, jwk, time, skew, jwt);
}


/**
 * cleanup jwt object
 */
void cjwt_destroy(cjwt_t *jwt)
{
    if (jwt) {
        if (jwt->header.kid) free(jwt->header.kid);
        if (jwt->header.private_headers) cJSON_Delete(jwt->header.private_headers);

        if (jwt->iss) free(jwt->iss);
        if (jwt->sub) free(jwt->sub);
        if (jwt->jti) free(jwt->jti);
        if (jwt->exp) free(jwt->exp);
        if (jwt->nbf) free(jwt->nbf);
        if (jwt->iat) free(jwt->iat);

        for (int i = 0; i < jwt->aud.count; i++) {
            if (jwt->aud.names[i]) {
                free(jwt->aud.names[i]);
            }
        }

        if (jwt->aud.names) free(jwt->aud.names);
        if (jwt->private_claims) cJSON_Delete(jwt->private_claims);

        free(jwt);
    }
}


cjwt_code_t cjwt_alg_string_to_enum(const char *s, size_t len, cjwt_alg_t *alg)
{
    char buf[6];
    int found = 0;

    if (!s || !len || !alg) {
        return CJWTE_INVALID_PARAMETERS;
    }

    if (SIZE_MAX == len) {
        len = strlen(s);
    }

    if ((4 != len) && (5 != len) && (18 != len)) {
        return CJWTE_UNKNOWN_ALG;
    }

    memcpy(buf, s, (len > 5) ? 5 : len);
    buf[(len > 5) ? 5 : len] = '\0';

    if (len > 5) {
        /* PBES2 algorithms are longer than 5 chars, so we need to handle them specially */
        char *long_buf = malloc(len + 1);
        if (!long_buf) return CJWTE_OUT_OF_MEMORY;
        memcpy(long_buf, s, len);
        long_buf[len] = '\0';
        found = alg_to_enum(long_buf, alg);
        free(long_buf);
    } else {
        found = alg_to_enum(buf, alg);
    }

    return (0 == found) ? CJWTE_OK : CJWTE_UNKNOWN_ALG;
}

static cJSON *construct_header_json(const cjwt_t *jwt)
{
    cJSON *header = cJSON_CreateObject();
    cJSON *tmp = NULL;
    if (!header) return NULL;

    cJSON_AddStringToObject(header, "alg", alg_to_string(jwt->header.alg));
    if (jwt->header.enc != enc_unknown) {
        cJSON_AddStringToObject(header, "enc", enc_to_string(jwt->header.enc));
    }
    cJSON_AddStringToObject(header, "typ", "JWT");

    if (jwt->header.kid) {
        cJSON_AddStringToObject(header, "kid", jwt->header.kid);
    }

    if (jwt->header.private_headers) {
        cJSON_ArrayForEach(tmp, jwt->header.private_headers) {
            cJSON_AddItemToObject(header, tmp->string, cJSON_Duplicate(tmp, true));
        }
    }
    return header;
}

static cJSON *construct_payload_json(const cjwt_t *jwt)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) return NULL;

    if (jwt->iss) cJSON_AddStringToObject(payload, "iss", jwt->iss);
    if (jwt->sub) cJSON_AddStringToObject(payload, "sub", jwt->sub);
    if (jwt->jti) cJSON_AddStringToObject(payload, "jti", jwt->jti);
    if (jwt->exp) cJSON_AddNumberToObject(payload, "exp", (double) *jwt->exp);
    if (jwt->nbf) cJSON_AddNumberToObject(payload, "nbf", (double) *jwt->nbf);
    if (jwt->iat) cJSON_AddNumberToObject(payload, "iat", (double) *jwt->iat);

    if (jwt->aud.count > 0) {
        if (jwt->aud.count == 1) {
            cJSON_AddStringToObject(payload, "aud", jwt->aud.names[0]);
        } else {
            cJSON *auds = cJSON_CreateArray();
            for (int i = 0; i < jwt->aud.count; i++) {
                cJSON_AddItemToArray(auds, cJSON_CreateString(jwt->aud.names[i]));
            }
            cJSON_AddItemToObject(payload, "aud", auds);
        }
    }

    if (jwt->private_claims) {
        cJSON *tmp = NULL;
        cJSON_ArrayForEach(tmp, jwt->private_claims) {
            cJSON_AddItemToObject(payload, tmp->string, cJSON_Duplicate(tmp, true));
        }
    }

    return payload;
}

cjwt_code_t cjwt_encode(const cjwt_t *jwt, const uint8_t *key, size_t key_len, char **output)
{
    cjwt_code_t rv = CJWTE_OK;
    cJSON *h_json = NULL, *p_json = NULL;
    char *h_str = NULL, *p_str = NULL;
    char *h_b64 = NULL, *p_b64 = NULL;
    char *full_data = NULL;
    uint8_t *sig = NULL;
    size_t sig_len = 0;
    char *sig_b64 = NULL;

    if (!jwt || !output) return CJWTE_INVALID_PARAMETERS;
    
    if (jwt->header.enc != enc_unknown) {
        rv = jwe_pbes2_prepare((cjwt_t *)jwt);
        if (rv != CJWTE_OK) return rv;
    }

    h_json = construct_header_json(jwt);
    p_json = construct_payload_json(jwt);
    if (!h_json || !p_json) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }

    h_str = cJSON_PrintUnformatted(h_json);
    p_str = cJSON_PrintUnformatted(p_json);
    if (!h_str || !p_str) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }

    h_b64 = b64url_encode_with_alloc((const uint8_t *)h_str, strlen(h_str), NULL);
    if (!h_b64) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }

    if (jwt->header.enc != enc_unknown) {
        rv = jwe_encrypt(jwt, h_b64, (const uint8_t *)p_str, strlen(p_str), key, key_len, NULL, output);
        goto cleanup;
    }

    p_b64 = b64url_encode_with_alloc((const uint8_t *)p_str, strlen(p_str), NULL);
    if (!p_b64) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }

    size_t full_len = strlen(h_b64) + 1 + strlen(p_b64);
    full_data = malloc(full_len + 1);
    if (!full_data) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }
    sprintf(full_data, "%s.%s", h_b64, p_b64);

    if (jwt->header.alg != alg_none) {
        rv = jws_sign(jwt->header.alg, (const uint8_t *)full_data, full_len, key, key_len, &sig, &sig_len);
        if (rv != CJWTE_OK) goto cleanup;

        sig_b64 = b64url_encode_with_alloc(sig, sig_len, NULL);
        if (!sig_b64) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }

        *output = malloc(full_len + 1 + strlen(sig_b64) + 1);
        if (!*output) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }
        sprintf(*output, "%s.%s", full_data, sig_b64);
    } else {
        *output = malloc(full_len + 2);
        if (!*output) { rv = CJWTE_OUT_OF_MEMORY; goto cleanup; }
        sprintf(*output, "%s.", full_data);
    }

cleanup:
    if (h_json) cJSON_Delete(h_json);
    if (p_json) cJSON_Delete(p_json);
    if (h_str) free(h_str);
    if (p_str) free(p_str);
    if (h_b64) free(h_b64);
    if (p_b64) free(p_b64);
    if (full_data) free(full_data);
    if (sig) free(sig);
    if (sig_b64) free(sig_b64);

    return rv;
}

static cjwt_kty_t string_to_kty(const char *s)
{
    if (!strcmp(s, "RSA")) return CJWT_KTY_RSA;
    if (!strcmp(s, "EC"))  return CJWT_KTY_EC;
    if (!strcmp(s, "oct")) return CJWT_KTY_OCT;
    if (!strcmp(s, "OKP")) return CJWT_KTY_OKP;
    return CJWT_KTY_UNKNOWN;
}

cjwt_code_t cjwt_jwk_parse(const char *json_str, cjwt_jwk_t **jwk)
{
    cJSON *json = NULL;
    cjwt_jwk_t *out = NULL;
    cjwt_code_t rv = CJWTE_OK;

    if (!json_str || !jwk) return CJWTE_INVALID_PARAMETERS;

    json = cJSON_Parse(json_str);
    if (!json) return CJWTE_HEADER_INVALID_JSON;

    out = calloc(1, sizeof(cjwt_jwk_t));
    if (!out) { cJSON_Delete(json); return CJWTE_OUT_OF_MEMORY; }

    cJSON *kty = cJSON_GetObjectItemCaseSensitive(json, "kty");
    if (kty && cJSON_IsString(kty)) {
        out->kty = string_to_kty(kty->valuestring);
    }

    rv |= process_string(json, "kid", &out->kid);
    rv |= process_string(json, "use", &out->use);
    rv |= process_string(json, "alg", &out->alg);

    if (rv != CJWTE_OK) {
        cjwt_jwk_destroy(out);
        cJSON_Delete(json);
        return rv;
    }

    out->key_json = json;
    *jwk = out;

    return CJWTE_OK;
}

void cjwt_jwk_destroy(cjwt_jwk_t *jwk)
{
    if (jwk) {
        if (jwk->kid) free(jwk->kid);
        if (jwk->use) free(jwk->use);
        if (jwk->alg) free(jwk->alg);
        if (jwk->key_json) cJSON_Delete(jwk->key_json);
        free(jwk);
    }
}

cjwt_code_t cjwt_jwks_parse(const char *json_str, cjwt_jwks_t **jwks)
{
    cJSON *json = NULL, *keys = NULL, *key = NULL;
    cjwt_jwks_t *out = NULL;
    cjwt_code_t rv = CJWTE_OK;

    if (!json_str || !jwks) return CJWTE_INVALID_PARAMETERS;

    json = cJSON_Parse(json_str);
    if (!json) return CJWTE_HEADER_INVALID_JSON;

    keys = cJSON_GetObjectItemCaseSensitive(json, "keys");
    if (!keys || !cJSON_IsArray(keys)) {
        cJSON_Delete(json);
        return CJWTE_HEADER_INVALID_JSON;
    }

    out = calloc(1, sizeof(cjwt_jwks_t));
    if (!out) { cJSON_Delete(json); return CJWTE_OUT_OF_MEMORY; }

    out->count = cJSON_GetArraySize(keys);
    out->keys = calloc(out->count, sizeof(cjwt_jwk_t *));
    if (!out->keys) {
        free(out);
        cJSON_Delete(json);
        return CJWTE_OUT_OF_MEMORY;
    }

    int i = 0;
    cJSON_ArrayForEach(key, keys) {
        char *key_str = cJSON_PrintUnformatted(key);
        if (key_str) {
            rv = cjwt_jwk_parse(key_str, &out->keys[i++]);
            free(key_str);
            if (rv != CJWTE_OK) break;
        } else {
            rv = CJWTE_OUT_OF_MEMORY;
            break;
        }
    }

    if (rv != CJWTE_OK) {
        cjwt_jwks_destroy(out);
        cJSON_Delete(json);
        return rv;
    }

    cJSON_Delete(json);
    *jwks = out;
    return CJWTE_OK;
}

void cjwt_jwks_destroy(cjwt_jwks_t *jwks)
{
    if (jwks) {
        for (int i = 0; i < jwks->count; i++) {
            cjwt_jwk_destroy(jwks->keys[i]);
        }
        free(jwks->keys);
        free(jwks);
    }
}
